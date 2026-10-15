#include "FloatWidget.h"

#include "AppIcon.h"
#include "DetailDialog.h"
#include "SettingsDialog.h"
#include "TopPanel.h"
#include "UiTheme.h"
#include "core/AutoStart.h"
#include "core/FullScreenDetect.h"
#include "core/PacketSource.h"
#include "core/ProcTraffic.h"
#include "core/Settings.h"
#include "core/StatsDb.h"
#include "core/StatsRecorder.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDate>
#include <QDateTime>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QScreen>
#include <QSettings>
#include <QSystemTrayIcon>

namespace {
const int kCardH = 58;

// 卡片宽度不再写死：按"最宽可能读数"（9.99 MB/s 这类）实测字体宽度算出来。
// 写死 150 时右侧会空一大截；凭感觉改小又可能把单位裁掉（drawRateRow 的文字排布
// 是"文本自 body.left()+20 起、单元左端 = 文本起点 + numW + 4"）。
int measureCardW()
{
    QFont fv(ts::fontFamily());
    fv.setPointSize(10);
    fv.setWeight(QFont::Medium);
    QFont fu(ts::fontFamily());
    fu.setPointSize(8);
    const QFontMetrics fmv(fv);
    const QFontMetrics fmu(fu);

    int numW = 0;
    for (const QString& s : {QStringLiteral("9.99"), QStringLiteral("999")})
        numW = qMax(numW, fmv.horizontalAdvance(s));
    int unitW = 0;
    for (const QString& s : {QStringLiteral("MB/s"), QStringLiteral("GB/s"),
                             QStringLiteral("TB/s"), QStringLiteral("KB/s")})
        unitW = qMax(unitW, fmu.horizontalAdvance(s));

    // body 左右各 12 + 文本起点 20 + numW + 4（数字与单位间距）+ unitW + 右侧 12 内边距 + 2 余量
    return 12 + 20 + numW + 4 + unitW + 12 + 2;
}

// 收起判定的容差：比"弹开"判定（只认可见卡片本身）宽出一圈，等于窗口比卡片多出的
// 阴影留白。两者用同一条边界时，光标贴着浮框/屏幕边缘小幅来回移动会被抖成
// "反复贴边再出来"；留一圈容差后只有真正离开浮框（等同于离开浮框窗口）才收回。
const int kHoverSlack = ts::kShadowPad;

const int kEdgeGap = 0;         // 吸附后卡片与屏幕边缘的间距（0=完全贴边）
const int kPeek = 14;           // 贴边隐藏时露出的宽度/高度
const int kSnapThreshold = 28;  // 松手时进入吸附的判定距离
const int kMargin = 20;         // 默认位置距屏幕边缘的间距

// 全屏探测间隔：250ms 内一定能响应（进/出全屏）。每次只做几次 Win32 取值，
// 相对抓包解析（每秒几千个包）可以忽略；想更快/更省直接改这一个常量。
const int kFsPollMs = 250;

// 单行：彩色三角 + 加粗数字 + 弱化单位
void drawRateRow(QPainter& p, const QRect& row, bool up, quint64 rate)
{
    const QColor accent = up ? ts::cUp() : ts::cDown();
    const bool idle = (rate == 0);
    const QColor valueCol = idle ? ts::cTextSub() : accent;
    const QColor unitCol = idle ? ts::cTextSub()
                                : QColor(accent.red(), accent.green(), accent.blue(), 170);

    ts::drawArrow(p, QPointF(row.left() + 6.0, row.center().y()), 11.0, valueCol, up);

    QString num, unit;
    ts::splitNumberUnit(formatRate(rate), num, unit);

    QFont fv(ts::fontFamily());
    fv.setPointSize(10);
    fv.setWeight(QFont::Medium);
    QFont fu(ts::fontFamily());
    fu.setPointSize(8);

    const int x = row.left() + 20;
    p.setFont(fv);
    const int numW = QFontMetrics(fv).horizontalAdvance(num);

    p.setPen(valueCol);
    p.drawText(QRect(x, row.top(), numW + 2, row.height()),
               Qt::AlignVCenter | Qt::AlignRight, num);

    p.setFont(fu);
    p.setPen(unitCol);
    p.drawText(QRect(x + numW + 4, row.top(), row.width() - numW - 24, row.height()),
               Qt::AlignVCenter | Qt::AlignLeft, unit);
}

// 速率 -> 条形填充比例（固定档位：0 / 100KB/s / 1MB/s / 10MB/s）
double levelFrac(quint64 rate)
{
    if (rate == 0)                        return 0.0;
    if (rate < 100ull * 1024ull)          return 0.25;
    if (rate < 1024ull * 1024ull)         return 0.5;
    if (rate < 10ull * 1024ull * 1024ull) return 0.75;
    return 1.0;
}
}  // namespace

FloatWidget::FloatWidget(QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool
                          | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint)
    , m_store(TrafficStore::instance())
{
    setAttribute(Qt::WA_TranslucentBackground);
    m_cardW = measureCardW();
    setFixedSize(m_cardW + ts::kShadowPad * 2, kCardH + ts::kShadowPad * 2);
    setMouseTracking(true);

    m_panel = new TopPanel(nullptr);
    m_panel->installEventFilter(this);
    m_panel->hide();
    // 面板要"单击浮框与面板之外的地方"就关：装一个应用级过滤器，把本程序内
    // 任何位置的按下事件都过一遍（事件先到过滤器、再到目标控件，见 eventFilter）
    qApp->installEventFilter(this);
    connect(m_panel, &TopPanel::detailRequested, this, &FloatWidget::showDetail);

    m_fade = new QPropertyAnimation(m_panel, "windowOpacity", this);
    m_fade->setDuration(150);
    connect(m_fade, &QPropertyAnimation::finished, this, [this]() {
        if (m_panel && m_panel->windowOpacity() < 0.02)
            m_panel->hide();
    });

    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(400);
    connect(&m_hideTimer, &QTimer::timeout, this, &FloatWidget::hidePanel);

    m_hoverAnim.setDuration(160);
    m_hoverAnim.setStartValue(0.0);
    m_hoverAnim.setEndValue(1.0);
    connect(&m_hoverAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
                m_hover = v.toDouble();
                update();
            });

    m_slide = new QPropertyAnimation(this, "pos", this);
    m_slide->setDuration(160);
    m_slide->setEasingCurve(QEasingCurve::OutCubic);

    // 刷新节拍：用户在设置里改（settings.ini 的 [Float] refreshMs，默认 1000ms）
    m_refreshMs = ts::floatRefreshMs();
    m_timer.setInterval(m_refreshMs);
    connect(&m_timer, &QTimer::timeout, this, &FloatWidget::onTick);
    m_timer.start();

    // 连接表快照独立于 UI 刷新节拍（m_refreshMs，默认 1s）：短命连接
    //（DNS/QUIC/秒进秒出的 HTTP）只有靠更高的采样率才抓得住，
    // 配合 ProcTraffic 的端口级记忆形成双保险。这个周期不跟随用户的刷新设置。
    m_procTimer.setInterval(300);
    connect(&m_procTimer, &QTimer::timeout, this, []() {
        ProcTraffic::instance().refresh();
    });
    ProcTraffic::instance().refresh();   // 立即建一次快照，避免启动初期流量全落"未知进程"
    m_procTimer.start();

    // 全屏探测：只判状态（前台窗口矩形 + 系统状态），不做数据采集
    m_hideOnFullscreen = ts::hideOnFullscreen();
    m_autoEdgeHide = ts::autoEdgeHide();
    m_topMostWanted = (windowFlags() & Qt::WindowStaysOnTopHint) != 0;
    m_fsTimer.setInterval(kFsPollMs);
    connect(&m_fsTimer, &QTimer::timeout, this, [this]() {
        checkFullscreen();
        // 贴边态的收尾复核：开合本来全靠 Enter/Leave 事件，一旦漏掉一个 Leave
        //（拖动被夹在屏幕边界后松手、光标已经跑到界外/任务栏里，系统那边可能根本不发），
        // 浮框就会一直弹开着收不回去。这里每 kFsPollMs 用真实光标坐标复核一次，
        // 判据与 restoreEdgePeek 完全一致，只是把"事件驱动"补成"事件 + 周期性复核"，
        // 不会多收。面板开着时不插手，面板那条路径仍按 leaveEvent 的 400ms 计时收尾。
        if (m_snapped && m_hovered && !m_panelShown)
            restoreEdgePeek();
    });
    m_fsTimer.start();

    loadPosition();   // 恢复上次位置；无记录时默认屏幕右下角

    setupTray();      // 托盘图标（不支持时内部直接跳过）

    // 主题切换：浮框自身全是手绘（每次 paint 取当前主题），重画即可；
    // 但卡片宽是按字体实测出来的，换字体后要重算（否则宽字体会把单位挤掉）
    connect(&ts::themeNotifier(), &ts::ThemeNotifier::changed, this, [this]() {
        m_cardW = measureCardW();
        setFixedSize(m_cardW + ts::kShadowPad * 2, kCardH + ts::kShadowPad * 2);
        if (m_snapped && !m_dragging) {   // 贴边态按新宽度重新归位
            const QRect avail = availGeometry();
            m_restPos = edgeRestPos(avail, m_edge, m_restPos);
            move(m_hovered ? m_restPos : edgeHidePos(avail, m_edge, m_restPos));
        }
        update();
        if (m_tray)
            m_tray->setIcon(trayIcon());   // 图标是按主题色现画的，换主题要重画
        if (m_panel)
            m_panel->update();
    });
}

FloatWidget::~FloatWidget()
{
    delete m_detail;     // 详情弹窗无父对象（独立窗口、不跟随置顶），需手动释放
    delete m_settings;   // 设置弹窗同上
}

void FloatWidget::onTick()
{
    // 连接表快照不在这里刷新：它由 m_procTimer（300ms）驱动，见构造函数

    // 域名快照仍要每拍计算：它同时负责 2h 条目回收与全局速率（totals 的数据源）。
    // 面板只显示进程榜，域名数据由详情弹窗按需读取（命中 500ms 缓存）
    m_store.snapshot(20, 0);
    const QVector<ProcStat> procs = m_store.procSnapshot(20);
    m_store.totals(m_upRate, m_downRate);
    if (m_panel && m_panelShown) {
        m_panel->setProcSnapshot(procs);
        m_panel->setTotals(m_downRate);
        updateUsage();
    }
    update();
}

// 「今日用量」摘要：查 SQLite，10 秒一次（面板每拍都刷，跟拍查库太浪费）。
// 查之前先 flushNow() 把当前增量写进同一小时桶，否则最多会差一个落盘周期（60s）。
// 峰值速率不在这里 —— 它由详情弹窗自己从 TrafficStore::peaks() 取。
void FloatWidget::updateUsage()
{
    if (!m_panel)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastUsageMs > 0 && now - m_lastUsageMs < 10000)
        return;
    m_lastUsageMs = now;

    StatsDb& db = StatsDb::instance();
    if (!db.isOpen()) {
        m_panel->setUsage(0, 0);
        return;
    }

    StatsRecorder::instance().flushNow();

    const QDate today = QDate::currentDate();
    const qint64 nowTs = QDateTime::currentSecsSinceEpoch();
    const qint64 dayStart = QDateTime(today, QTime(0, 0)).toSecsSinceEpoch();
    const StatsDb::Totals todayT = db.totals(dayStart, nowTs, true);
    m_panel->setUsage(todayT.down, todayT.up);
}

void FloatWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);

    // 浮框是置顶无边框窗：贴边时窗口必有一部分落到桌面可用区之外（贴底那一片正好是任务栏）。
    // 不裁的话整块卡片/阴影会被实画在任务栏上（看起来"整个组件糊到任务栏上"）。
    // 这里按可用区裁一刀，贴底隐藏时就只剩露出的那一条；位置计算不受影响。
    if (m_snapped) {
        // 屏幕判定三级回退（同 positionPanel）：基准用浮框自身，不能用光标 ——
        // 光标可能已经跑到别的显示器上，用错屏的可用区会把浮框整条裁没。
        QScreen* s = QGuiApplication::screenAt(pos() + QPoint(width() / 2, height() / 2));
        if (!s)
            s = QGuiApplication::screenAt(QCursor::pos());
        if (!s)
            s = QGuiApplication::primaryScreen();
        const QRect vis = s ? (rect() & s->availableGeometry().translated(-pos()))
                            : rect();
        if (vis.isEmpty())
            return;
        p.setClipRect(vis);
    }

    ts::drawCard(p, rect(), ts::kCardRadius,
                 ts::blend(ts::cCard(), ts::cCardHover(), m_hover),
                 ts::blend(ts::cBorder(), ts::cBorderHover(), m_hover));

    const QRect card = rect().adjusted(ts::kShadowPad, ts::kShadowPad,
                                       -ts::kShadowPad, -ts::kShadowPad);

    // 吸附态且鼠标不在浮框上：只画露出屏幕的那一条（上下行占比条）
    if (m_snapped && !m_hovered) {
        drawEdgeBar(p, card);
        return;
    }

    const QRect body = card.adjusted(12, 0, -12, 0);
    const int rowH = body.height() / 2;

    drawRateRow(p, QRect(body.left(), body.top(), body.width(), rowH), true, m_upRate);

    p.setPen(QPen(ts::cDivider(), 1.0));
    const int midY = body.top() + rowH;
    p.drawLine(QPointF(body.left(), midY), QPointF(body.right(), midY));

    drawRateRow(p, QRect(body.left(), midY, body.width(), body.height() - rowH),
                false, m_downRate);
}

void FloatWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        // 贴边隐藏态先归位到完整位置，再跟随鼠标，避免"拖着一条走"
        if (m_snapped && pos() != m_restPos) {
            m_slide->stop();
            move(m_restPos);
        }
        m_pressWinPos = pos();   // 松开时对比：位置没变过就是单击（切面板），变了才是拖动
        m_panelWasShown = m_panelShown;   // 记录拖之前的面板状态，供松手时判断要不要恢复
        m_dragging = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        m_dragPos = e->globalPosition().toPoint() - frameGeometry().topLeft();
#else
        m_dragPos = e->globalPos() - frameGeometry().topLeft();
#endif
    }
}

void FloatWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging) {
        // 非拖动：Enter 只在光标"进入窗口"时来一次，而窗口比卡片大一圈透明阴影留白，
        // 光标从留白挪到卡片上不会再补一次 Enter。enterEvent 只认卡片（留白不算进入），
        // 所以这里按真实坐标补一次"悬停进入"，否则会出现"光标明明在浮框看得见的地方、
        // 浮框却不滑出来"的死区。
        if (!m_hovered && cursorOnCard()) {
            m_hideTimer.stop();
            animateHover(true);
            m_hovered = true;
            if (m_snapped)
                slideTo(m_restPos);   // 贴边态：滑出完整卡片
        }
        return;
    }

    // 拖拽途中若左键已抬起（例如中途触发截图，松开事件被别的窗口吃掉），
    // 说明释放事件丢失，立即结束拖拽，避免浮框"粘"在光标上。
    if (!(e->buttons() & Qt::LeftButton)) {
        endDrag();
        return;
    }

    // 拖拽期间收起排行面板，避免面板随浮框移动、遮挡落点。
    // 放在这里而非 mousePressEvent：单击不移动时面板不会闪一下。
    if (m_panelShown)
        hidePanel();

    // 拖动全程夹在桌面可用区内（见 desktopAvail）：关掉「自动贴边隐藏」后松手没有
    // 吸附兜底，只在 endDrag 夹取的话拖动过程本身就能把浮框拖出屏幕、内容不可见。
    // 夹的是阴影留白，可视卡片严格不越界；贴边态下卡片仍能精确贴到边缘（距离 0），
    // 吸附阈值 kSnapThreshold 照常命中。
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint np = e->globalPosition().toPoint() - m_dragPos;
#else
    const QPoint np = e->globalPos() - m_dragPos;
#endif
    move(clampToAvail(desktopAvail(), np));
    if (m_panel && m_panel->isVisible())
        positionPanel();
}

void FloatWidget::mouseReleaseEvent(QMouseEvent* e)
{
    // 窗口位置是否被拖动过（拖动会实时 move，单击不会）
    const bool moved = (pos() != m_pressWinPos);
    endDrag();

    if (e->button() != Qt::LeftButton)
        return;

    if (moved) {
        // 拖动结束：只恢复"拖之前就开着"的面板（拖拽时收起的那些）。
        // 拖之前没开着就不再自动展开 —— 面板的开关只认单击。
        if (m_panelWasShown && underMouse() && m_panel && !m_panelShown) {
            showPanel();
            onTick();   // 隐藏期间 onTick 不刷新面板，恢复时补一次数据
        }
        return;
    }

    // 单击：开关进程榜（面板的关闭时机不止这一处，见 eventFilter / event）
    if (m_panelShown) {
        hidePanel();
    } else if (m_panel) {
        showPanel();
        onTick();       // 隐藏期间 onTick 不刷面板，展开时补一次数据
    }
}

// 拖拽收尾：贴边判定、落盘、以及吸附后退回隐藏位。
// 松开鼠标 / 中途丢键 / 窗口失焦统一走这里，保证 m_dragging 一定被复位。
void FloatWidget::endDrag()
{
    if (!m_dragging)
        return;
    m_dragging = false;

    const QRect avail = availGeometry();
    // 关掉「自动贴边隐藏」后拖到边缘也不吸附，浮框停在松手的位置
    const int edge = m_autoEdgeHide ? nearestEdge(avail) : 0;
    m_snapped = (edge != 0);
    m_edge = edge;

    if (m_snapped) {
        m_restPos = edgeRestPos(avail, edge, pos());
        move(m_restPos);
        // 只认真实光标坐标：松手事件里 Qt 仍认为"抓取者一直在鼠标下"，而浮框已被夹在
        // 屏幕边界（光标继续往外跑到界外/任务栏里），信 underMouse() 就会判成"还停在
        // 浮框上"、一直弹开着收不回去（而且系统那边不会再补 Leave 事件来纠正）。
        if (cursorOnCard()) {
            m_hovered = true;      // 鼠标还停在浮框上，先不收回去
        } else {
            m_hovered = false;
            slideTo(edgeHidePos(avail, edge, m_restPos));
        }
    } else {
        // 关掉吸附后松手没有兜底：落点也夹回桌面内（丢键/失焦等中途结束拖拽的路径同走这里）
        m_restPos = clampToAvail(desktopAvail(), pos());
        if (m_restPos != pos())
            move(m_restPos);
    }
    savePosition();
}

bool FloatWidget::event(QEvent* ev)
{
    // 窗口失去激活（例如拖拽途中弹出截图工具覆盖层）时，鼠标释放事件可能丢失，
    // 主动结束拖拽，防止浮框黏着光标。
    if (ev->type() == QEvent::WindowDeactivate) {
        if (m_dragging)
            endDrag();
        if (m_panelShown)
            checkPanelFocus();   // 点到别的程序/桌面 → 关面板
    }
    return QWidget::event(ev);
}

// 失活后延迟一拍再判断：激活状态可能在事件处理过程中才转移完
//（点面板时浮框会先失活、面板随后才成为活动窗口）。
void FloatWidget::checkPanelFocus()
{
    // 失活后延迟一拍再判断的延时（见上方注释）
    constexpr int kFocusCheckDelayMs = 150;
    QTimer::singleShot(kFocusCheckDelayMs, this, [this]() {
        if (!m_panel || !m_panelShown)
            return;
        if (m_menuOpen)   // 右键菜单自己也算"失活"的来源，别把面板一起关了
            return;
        QWidget* aw = QApplication::activeWindow();
        // 焦点还在浮框/面板上（含面板内部点击、拖动过程）就不动它；
        // 焦点落到本程序之外时 activeWindow() 为 nullptr，正是"点了别处"。
        if (aw == this || aw == m_panel)
            return;
        hidePanel();
    });
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void FloatWidget::enterEvent(QEnterEvent*)
#else
void FloatWidget::enterEvent(QEvent*)
#endif
{
    // 透明阴影留白不算"进入浮框"：窗口比可见卡片四周各大 kShadowPad，光标只落在留白上
    //（贴边时这圈留白压在屏幕边缘）不该把贴边条弹开；光标随后挪到卡片上时由
    // mouseMoveEvent 补进来，不会留下死区。
    if (!cursorOnCard())
        return;
    m_hideTimer.stop();
    animateHover(true);
    m_hovered = true;
    if (m_snapped && !m_dragging)
        slideTo(m_restPos);        // 贴边态：滑出完整卡片
    // 面板**不再**随悬停展开（2026-10-11 用户要求）：只在单击浮框时开合
}

void FloatWidget::leaveEvent(QEvent*)
{
    animateHover(false);
    if (!m_snapped) {
        // 普通态：面板按点击开关，鼠标移开不动它；但悬停标记要跟着复位，
        // 否则"从透明留白进入 → 再挪到卡片上"时 highlight 补不回来（见 mouseMoveEvent）。
        m_hovered = false;
        return;
    }
    if (m_menuOpen)
        return;                    // 右键菜单盖住光标造成的 Leave：什么都不做

    // 吸附态本来就是"鼠标一走就缩回窄条"：面板开着时先给 400ms
    // 让光标能移到面板上（面板 Enter 会取消倒计时），否则直接滑回去。
    if (m_panelShown)
        m_hideTimer.start();
    else
        restoreEdgePeek();
}

bool FloatWidget::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_panel) {
        if (ev->type() == QEvent::Enter)
            m_hideTimer.stop();
        else if (ev->type() == QEvent::Leave) {
            if (m_snapped)         // 同上：只有吸附态才因移开鼠标而收面板
                m_hideTimer.start();
        }
    } else if (m_panelShown && ev->type() == QEvent::MouseButtonPress) {
        // 单击浮框与面板之外的地方 → 关面板。窗口失活（点到别的程序）的那一路
        // 由 event() 的 WindowDeactivate 兜底，两条合起来覆盖"单击其他地方"。
        auto* me = static_cast<QMouseEvent*>(ev);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint gp = me->globalPosition().toPoint();
#else
        const QPoint gp = me->globalPos();
#endif
        if (!frameGeometry().contains(gp)
            && !(m_panel && m_panel->frameGeometry().contains(gp)))
            hidePanel();
    }
    return QWidget::eventFilter(obj, ev);
}

void FloatWidget::animateHover(bool in)
{
    m_hoverAnim.stop();
    m_hoverAnim.setDirection(in ? QAbstractAnimation::Forward
                               : QAbstractAnimation::Backward);
    m_hoverAnim.start();
}

void FloatWidget::showPanel()
{
    if (!m_panel)
        return;
    m_panelShown = true;
    // 面板刚展开时强制刷新一次用量（节流标记清零），否则会先显示上一次的旧读数
    m_lastUsageMs = 0;
    updateUsage();
    positionPanel();
    m_fade->stop();
    m_panel->setWindowOpacity(0.0);
    m_panel->show();
    m_fade->setStartValue(0.0);
    m_fade->setEndValue(1.0);
    m_fade->start();
    // 拿住激活：这样用户点到别的程序/桌面时本窗口会收到 WindowDeactivate，
    // 「单击其他地方关掉浮框」才有信号可依（本程序内的点击由 eventFilter 截获）。
    activateWindow();
}

void FloatWidget::hidePanel()
{
    if (!m_panel || !m_panelShown)
        return;
    m_panelShown = false;
    m_hideTimer.stop();
    m_fade->stop();
    m_fade->setStartValue(m_panel->windowOpacity());
    m_fade->setEndValue(0.0);
    m_fade->start();
    restoreEdgePeek();
}

// 吸附态收尾：鼠标已离开浮框时滑回贴边位，只留一条占比窄条。
// 「光标还在浮框上就不收」这道判断不能省：单击关面板时人还在浮框上（hidePanel 也会走到
// 这里），少了它就会"面板一关、浮框跟着缩回侧边"，而光标明明还停在上面。
void FloatWidget::restoreEdgePeek()
{
    if (m_menuOpen)
        return;                        // 右键菜单开着：不收，等菜单关掉再说
    if (!m_snapped || m_dragging || !m_hovered)
        return;
    // 判据只认**可见卡片**上的真实光标坐标（underMouse() 在抓取/弹层场景会失真，
    // 拖动期间 Qt 认为抓取者一直在鼠标下），并留一圈 kHoverSlack 容差：
    // 弹开只认可见卡片本身、收起要等光标离开卡片一整圈阴影留白——两边同一条边界时，
    // 光标贴着浮框/屏幕边缘小幅来回移动就会被抖成"反复贴边再出来"。
    // 面板开着时光标在面板上也算"还在浮框上"：面板是浮框的延伸，不该把浮框收回窄条。
    if (cursorOnCard(kHoverSlack)
        || (m_panel && m_panelShown
            && m_panel->frameGeometry().contains(QCursor::pos())))
        return;
    m_hovered = false;
    slideTo(edgeHidePos(availGeometry(), m_edge, m_restPos));
}

void FloatWidget::positionPanel()
{
    if (!m_panel)
        return;

    // 吸附隐藏态下 pos() 是"贴边隐藏位"（浮框大半在屏幕外），
    // 必须用完整显示位 m_restPos 作基准，否则面板会被摆到屏幕外；拖拽中仍跟随 pos()。
    const QPoint anchor = (m_snapped && !m_dragging) ? m_restPos : pos();

    // 屏幕判定三级回退：基准卡中心 -> 光标所在屏 -> 主屏
    // （吸附态下浮框中心点可能落在屏幕外，screenAt 会返回 null 导致夹取被跳过）
    QScreen* s = QGuiApplication::screenAt(
        anchor + QPoint(width() / 2, height() / 2));
    if (!s)
        s = QGuiApplication::screenAt(QCursor::pos());
    if (!s)
        s = QGuiApplication::primaryScreen();

    // 浮框与面板使用相同的阴影留白，卡片左边缘自然对齐，纵向留 6px 间距
    QPoint p = anchor;
    p.setY(p.y() + kCardH + 6);
    if (s) {
        const QRect avail = s->availableGeometry();
        if (p.x() + m_panel->width() - ts::kShadowPad > avail.right())
            p.setX(avail.right() - m_panel->width() + ts::kShadowPad);
        if (p.x() + ts::kShadowPad < avail.left())
            p.setX(avail.left() - ts::kShadowPad);
        if (p.y() + m_panel->height() - ts::kShadowPad > avail.bottom())
            p.setY(qMax(avail.top() - ts::kShadowPad,
                        anchor.y() + 2 * ts::kShadowPad - 6 - m_panel->height()));
    }
    m_panel->move(p);
}

// ---------------------------------------------------------------------------
// 位置记忆 + 边缘吸附
// ---------------------------------------------------------------------------

QRect FloatWidget::availGeometry() const
{
    QScreen* s = QGuiApplication::screenAt(QCursor::pos());
    if (!s)
        s = QGuiApplication::primaryScreen();
    return s ? s->availableGeometry() : QRect(0, 0, 1920, 1080);
}

// 拖动夹取用的"桌面可用区"：所有屏幕可用区的并集。
// 不用 availGeometry()（鼠标所在屏）是因为拖动时鼠标一直贴着浮框走，一越过屏边界
// 基准屏就会切换，夹取结果会让浮框突然一跳；并集在多屏并排时是连续的，可以正常跨屏拖动。
QRect FloatWidget::desktopAvail() const
{
    QRect r;
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* s : screens) {
        if (!s)
            continue;
        r = r.isNull() ? s->availableGeometry() : r.united(s->availableGeometry());
    }
    return r.isNull() ? availGeometry() : r;
}

// 可见卡片在当前窗口坐标下的矩形（窗口比卡片四周各多一圈透明阴影留白）
QRect FloatWidget::cardRect() const
{
    return rect().adjusted(ts::kShadowPad, ts::kShadowPad,
                           -ts::kShadowPad, -ts::kShadowPad);
}

// 光标是否真的停在**看得见的那部分卡片**上（slack>0 时再放宽一圈，见 kHoverSlack）。
// 三个坑都在这里绕开：
//   ① underMouse() 在拖动/弹层场景会失真（拖动期间 Qt 认为抓取者一直在鼠标下）；
//   ② 窗口比卡片四周各多一圈透明阴影留白，留白是看不见的，不算"在浮框上"
//      （贴边时这圈留白正好压在屏幕边缘、贴底那条落在任务栏里）；
//   ③ 贴边隐藏时卡片大半挪到屏幕外，屏幕外那片（例如任务栏里）虽然仍在卡片矩形内，
//      但用户根本看不见，也不该算悬停（否则贴底时从任务栏往上划会把它弹开又缩回）。
bool FloatWidget::cursorOnCard(int slack) const
{
    const QPoint gp = QCursor::pos();
    const QScreen* s = QGuiApplication::screenAt(gp);
    if (!s)
        s = QGuiApplication::primaryScreen();
    QRect visible = cardRect().translated(pos());   // 卡片在屏幕坐标系下的矩形
    if (s)
        visible &= s->availableGeometry();          // 只保留屏幕内的可见部分
    if (slack > 0)
        visible.adjust(-slack, -slack, slack, slack);
    return visible.contains(gp);
}

void FloatWidget::loadPosition()
{
    const QRect avail = availGeometry();
    QSettings st = ts::appSettings();
    const int edge = st.value(QStringLiteral("Float/edge"), 0).toInt();
    const QPoint saved = st.value(QStringLiteral("Float/pos")).toPoint();

    // 关掉「自动贴边隐藏」时不恢复吸附态：按普通位置处理，始终完整显示
    if (m_autoEdgeHide && edge >= 1 && edge <= 4) {
        m_edge = edge;
        m_snapped = true;
        m_restPos = edgeRestPos(avail, edge, saved.isNull() ? pos() : saved);
    } else if (!saved.isNull()) {
        // 分辨率/显示器变化时夹回可见区：用多屏并集，避免那块屏已被拔掉时把浮框
        // 恢复到不存在的坐标上（此时 availGeometry 只看鼠标所在屏，判不出来）
        m_restPos = clampToAvail(desktopAvail(), saved);
    } else {
        // 默认落在屏幕右下角
        m_restPos = QPoint(avail.right() - width() + ts::kShadowPad - kMargin,
                           avail.bottom() - height() + ts::kShadowPad - kMargin);
    }
    move(m_snapped ? edgeHidePos(avail, m_edge, m_restPos) : m_restPos);
}

void FloatWidget::savePosition() const
{
    // 走 setIni 而不是直接拿 QSettings 写：它是统一写入口，且负责 sync()。
    // 注意 ini 里不写注释（QSettings 整份重写会冲掉），见 core/Settings.h。
    ts::setIni(QStringLiteral("Float/pos"), m_restPos);
    ts::setIni(QStringLiteral("Float/edge"), m_snapped ? m_edge : 0);
}

QPoint FloatWidget::clampToAvail(const QRect& avail, const QPoint& p) const
{
    return QPoint(qBound(avail.left() - ts::kShadowPad, p.x(),
                         avail.right() - width() + ts::kShadowPad),
                  qBound(avail.top() - ts::kShadowPad, p.y(),
                         avail.bottom() - height() + ts::kShadowPad));
}

QPoint FloatWidget::edgeRestPos(const QRect& avail, int edge, const QPoint& hint) const
{
    int x = hint.x();
    int y = hint.y();
    switch (edge) {
    case 1:  x = avail.left() + kEdgeGap - ts::kShadowPad;             break;
    case 2:  x = avail.right() - kEdgeGap - m_cardW - ts::kShadowPad;  break;
    case 3:  y = avail.top() + kEdgeGap - ts::kShadowPad;              break;
    default: y = avail.bottom() - kEdgeGap - kCardH - ts::kShadowPad;  break;
    }
    return clampToAvail(avail, QPoint(x, y));
}

QPoint FloatWidget::edgeHidePos(const QRect& avail, int edge, const QPoint& rest) const
{
    QPoint p = rest;
    switch (edge) {
    case 1:  p.setX(avail.left() + kPeek - ts::kShadowPad - m_cardW); break;
    case 2:  p.setX(avail.right() - kPeek - ts::kShadowPad);          break;
    case 3:  p.setY(avail.top() + kPeek - ts::kShadowPad - kCardH);   break;
    default: p.setY(avail.bottom() - kPeek - ts::kShadowPad);         break;
    }
    return p;
}

int FloatWidget::nearestEdge(const QRect& avail) const
{
    const QRect card(x() + ts::kShadowPad, y() + ts::kShadowPad, m_cardW, kCardH);
    const int d[5] = {0,
                      card.left() - avail.left(),
                      avail.right() - card.right(),
                      card.top() - avail.top(),
                      avail.bottom() - card.bottom()};
    int best = 0;
    int bestD = kSnapThreshold + 1;
    for (int e = 1; e <= 4; ++e) {
        if (d[e] <= kSnapThreshold && d[e] < bestD) {
            bestD = d[e];
            best = e;
        }
    }
    return best;
}

QRect FloatWidget::peekRect(const QRect& card) const
{
    switch (m_edge) {
    case 1:  return QRect(card.right() - kPeek, card.top(), kPeek, card.height());
    case 2:  return QRect(card.left(), card.top(), kPeek, card.height());
    case 3:  return QRect(card.left(), card.bottom() - kPeek, card.width(), kPeek);
    default: return QRect(card.left(), card.top(), card.width(), kPeek);
    }
}

void FloatWidget::slideTo(const QPoint& target)
{
    m_slide->stop();
    m_slide->setStartValue(pos());
    m_slide->setEndValue(target);
    m_slide->start();
}

void FloatWidget::drawEdgeBar(QPainter& p, const QRect& card)
{
    const QRect peek = peekRect(card).adjusted(3, 3, -3, -3);
    if (peek.width() <= 0 || peek.height() <= 0)
        return;

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(peek), 4.0, 4.0);
    p.setClipPath(clip);
    p.setPen(Qt::NoPen);

    const double upFrac   = levelFrac(m_upRate);
    const double downFrac = levelFrac(m_downRate);
    const QColor groove = ts::theme().groove;   // 空闲时的底槽

    const int half = peek.height() / 2;
    const QRect upLane(peek.left(), peek.top(), peek.width(), half);
    const QRect downLane(peek.left(), peek.top() + half,
                         peek.width(), peek.height() - half);
    p.fillRect(upLane, groove);
    p.fillRect(downLane, groove);

    if (m_edge == 1 || m_edge == 2) {
        // 左右贴边：竖条上段=上传（自上而下生长）、下段=下载（自下而上生长）
        p.fillRect(QRect(upLane.left(), upLane.top(), upLane.width(),
                         int(upLane.height() * upFrac)), ts::cUp());
        const int hDown = int(downLane.height() * downFrac);
        p.fillRect(QRect(downLane.left(), downLane.top() + downLane.height() - hDown,
                         downLane.width(), hDown), ts::cDown());
    } else {
        // 上下贴边：横条上排=上传、下排=下载，均自左向右生长
        p.fillRect(QRect(upLane.left(), upLane.top(),
                         int(upLane.width() * upFrac), upLane.height()), ts::cUp());
        p.fillRect(QRect(downLane.left(), downLane.top(),
                         int(downLane.width() * downFrac), downLane.height()),
                   ts::cDown());
    }
    p.restore();
}

void FloatWidget::showSettings()
{
    if (!m_settings) {
        m_settings = new SettingsDialog(nullptr);   // 无父窗口：不跟随浮框的置顶属性
        connect(m_settings, &SettingsDialog::topMostToggled,
                this, &FloatWidget::setTopMost);
        connect(m_settings, &SettingsDialog::refreshMsChanged,
                this, &FloatWidget::setRefreshMs);
        connect(m_settings, &SettingsDialog::hideOnFullscreenToggled,
                this, &FloatWidget::setHideOnFullscreen);
        connect(m_settings, &SettingsDialog::autoEdgeHideToggled,
                this, &FloatWidget::setAutoEdgeHide);
    }
    // 置顶与刷新周期只有浮框自己知道（设置弹窗不是置顶窗口），显示前同步给它。
    // 置顶同步"用户意愿"而非实际窗口标志：全屏隐藏期间实际标志是临时摘掉的。
    m_settings->setTopMost(m_topMostWanted);
    m_settings->setHideOnFullscreen(m_hideOnFullscreen);
    m_settings->setAutoEdgeHide(m_autoEdgeHide);
    // 自启动状态只认计划任务的真实状态（用户在任务管理器里删了也能同步显示）
    m_settings->setAutoStart(ts::autoStartEnabled());
    m_settings->setRefreshMs(m_refreshMs);
    m_settings->setCountUnattributed(m_store.countsUnattributed());
    m_settings->show();
    m_settings->raise();
    m_settings->activateWindow();
}

// 改刷新周期：正在跑的定时器改 interval 即可（会按新周期重新计时），不用重启
void FloatWidget::setRefreshMs(int ms)
{
    const int v = ts::clampRefreshMs(ms);
    if (v == m_refreshMs)
        return;
    m_refreshMs = v;
    m_timer.setInterval(v);
    ts::setFloatRefreshMs(v);   // 落盘，下次启动沿用
}

void FloatWidget::setTopMost(bool on)
{
    m_topMostWanted = on;   // 只记用户意愿：全屏期间实际标志会被临时摘掉
    applyWindowState();
}

void FloatWidget::setHideOnFullscreen(bool on)
{
    m_hideOnFullscreen = on;
    ts::setHideOnFullscreen(on);
    checkFullscreen();      // 关掉时若正隐藏会立即恢复，打开时立即判定一次
}

// 「自动贴边隐藏」开关：落盘后立即生效。
//   关掉：正贴边就退出吸附态、回到完整显示位（否则会一直只剩屏幕边缘一条）；
//   打开：按当前位置重判一次，靠近边缘就立刻吸附（鼠标不在浮框上则收起）。
void FloatWidget::setAutoEdgeHide(bool on)
{
    m_autoEdgeHide = on;
    ts::setAutoEdgeHide(on);

    m_slide->stop();   // 可能有正在进行的贴边/滑出动画
    const QRect avail = availGeometry();

    if (!on && m_snapped) {
        m_snapped = false;
        m_edge = 0;
        m_hovered = true;      // 不再吸附即永远展开
        move(m_restPos);
        savePosition();
    } else if (on) {
        const int edge = nearestEdge(avail);
        m_snapped = (edge != 0);
        m_edge = edge;
        if (m_snapped) {
            m_restPos = edgeRestPos(avail, edge, pos());
            move(m_restPos);
            if (underMouse()) {
                m_hovered = true;      // 鼠标还停在浮框上，先不收回去
            } else {
                m_hovered = false;
                slideTo(edgeHidePos(avail, edge, m_restPos));
            }
            savePosition();
        }
    }
    update();
}

// 置顶标志与显隐的唯一出口：置顶 = 用户意愿 && 当前没有因全屏隐藏。
// 全屏时连置顶一起摘掉，避免无边框全屏 / 多屏场景下浮框仍压在全屏程序上面。
void FloatWidget::applyWindowState()
{
    Qt::WindowFlags f = windowFlags();
    f.setFlag(Qt::WindowStaysOnTopHint, m_topMostWanted && !m_fsHidden);
    if (f != windowFlags())
        setWindowFlags(f);   // 改标志会顺带隐藏窗口，下面按意图决定是否显示
    // 吸附态的位置由 Qt 保留，show() 出来还在原位
    if (m_fsHidden) {
        if (isVisible())
            hide();
    } else if (!isVisible()) {
        show();
    }
}

void FloatWidget::checkFullscreen()
{
    const bool fs = m_hideOnFullscreen && ts::isFullscreenAppActive();
    if (fs == m_fsHidden)
        return;
    m_fsHidden = fs;

    if (fs) {
        // 收尾：榜单淡出后强制收掉（别等动画，浮框马上就要隐藏），
        // 贴边滑动动画也要停，否则隐藏期间还在挪窗口。
        m_hideTimer.stop();
        hidePanel();
        if (m_panel) {
            m_fade->stop();
            m_panel->hide();
            m_panelShown = false;
        }
        m_slide->stop();
        m_hovered = false;
        m_hover = 0.0;
    }
    applyWindowState();

    // 隐藏期间统计照跑（抓包与落盘不受影响），回来时立刻补一次数字
    if (!fs)
        onTick();
}

void FloatWidget::showDetail()
{
    if (!m_detail)
        m_detail = new DetailDialog(nullptr);   // 无父窗口：不跟随浮框的置顶属性
    m_detail->show();
    m_detail->raise();
    m_detail->activateWindow();
}

// 菜单项固定为「查看详情 / 设置 / 退出」：网卡选择、重置统计、窗口置顶都收进设置弹窗。
// 浮框右键与托盘右键**共用这一份**，避免两边菜单各写一套、改一处漏一处；
// 详情项也因此出现在托盘右键上（浮框被全屏隐藏时它是进详情的唯一入口）。
void FloatWidget::fillMenu(QMenu& menu)
{
    ts::polishMenu(menu);   // 无边框 + 半透明，四角才会是真圆角（见 UiTheme.h）

    // 详情弹窗无父窗口、不跟随置顶，见 showDetail()
    menu.addAction(QStringLiteral("查看详情"), this, &FloatWidget::showDetail);
    menu.addAction(QStringLiteral("设置"), this, &FloatWidget::showSettings);
    menu.addSeparator();
    menu.addAction(QStringLiteral("退出"), this, []() {
        PacketSource::instance().stopCapture();
        QCoreApplication::quit();
    });
}

void FloatWidget::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu menu(this);
    fillMenu(menu);
    // 菜单是独立弹层、正好盖在光标下，浮框会因此收到 Leave：不标记的话
    // 吸附态会"一右击就缩回侧边，只剩菜单浮在那儿"（见 restoreEdgePeek / leaveEvent）。
    m_menuOpen = true;
    menu.exec(e->globalPos());
    m_menuOpen = false;
    // 菜单关掉后光标多半已不在浮框上（例如点完「设置」）：这时才按常规收回窄条
    if (m_snapped && !underMouse())
        restoreEdgePeek();
}

void FloatWidget::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(trayIcon());
    m_tray->setToolTip(QStringLiteral("TrafficScope"));

    // 右键：与浮框右键同一份菜单（Qt 会自己挂到托盘的右键上）
    QMenu* menu = new QMenu(this);
    fillMenu(*menu);
    m_tray->setContextMenu(menu);

    // 左键单击：打开设置（浮框被全屏隐藏时，托盘是唯一入口）
    connect(m_tray, &QSystemTrayIcon::activated,
            this, [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger)
                    showSettings();
            });
    m_tray->show();
}

// 托盘图标与窗口图标是同一枚（↑橙 ↓绿，见 ui/AppIcon.h），按当前主题色现画
QIcon FloatWidget::trayIcon() const
{
    return ts::appIcon();
}
