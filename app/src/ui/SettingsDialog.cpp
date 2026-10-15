#include "SettingsDialog.h"

#include "ConfirmCard.h"
#include "DropDown.h"
#include "ListPicker.h"
#include "TipCard.h"
#include "TitleBar.h"
#include "UiTheme.h"
#include "core/AutoStart.h"
#include "core/NativeHost.h"
#include "core/NativeHostReg.h"
#include "core/NetInterfacePicker.h"
#include "core/PacketSource.h"
#include "core/Settings.h"
#include "core/StatsRecorder.h"
#include "core/TrafficStore.h"

#include <algorithm>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

namespace {

const int kNavW = 132;                        // 左侧导航列宽
const int kNavItemH = 30;                     // 导航项高
const int kPadX = 18;                         // 页面左右内边距
const int kContentW = 384;                    // 右侧内容可用宽度（灰字据此省略）
// 卡片宽 = 导航列 + 页面左右内边距 + 内容宽（窗口 = 卡片 + 2 * 阴影留白）
const int kCardW = kNavW + kPadX * 2 + kContentW;
const int kRowH = 24;                         // 单选/复选/下拉行高
const int kLabelW = 76;                       // 行首标签列宽（多行对齐用）
const int kLabelH = 17;                       // 分组标题与灰字说明行高
const int kBtnH = 26;

// 所有色值都是 %TOKEN%，由 ts::themed() 按当前主题替换（见 UiTheme.h）
const char* kSettingsQss =
    // 卡片背景与圆角交给 paintEvent 手绘（顶层窗口透明，QSS 的背景不渲染）
    "QDialog { background: transparent; font-family: \"%FONT%\"; }"
    "#secTitle { color: %TEXT%; font-size: 12px; font-weight: 600; }"
    "#rowLabel { color: %TEXT2%; font-size: 12px; }"
    "#curDev { color: %TEXT_SUB%; font-size: 11px; }"
    "#hint { color: %TEXT_FAINT%; font-size: 11px; }"
    "#msg { color: %TEXT%; font-size: 12px; }"
    "#tipText { color: %TEXT%; font-size: 12px; }"
    "#divider { background: %POPUP_SEP%; }"
    // 左侧导航项：选中态 = 强调色文字 + 淡强调底 + 左侧 3px 强调竖条
    // （3px 竖条用 border-left 画，QSS 能直接支持，不必手绘）
    "#navItem { color: %TEXT2%; font-size: 12px; text-align: left;"
    " padding-left: 14px; background: transparent;"
    " border: none; border-left: 3px solid transparent; border-radius: 6px; }"
    "#navItem:hover { color: %TEXT%; background: %FIELD_BG%; }"
    "#navItem:checked { color: %ACCENT%; background: %ACCENT_SOFT2%;"
    " border-left-color: %ACCENT%; }"
    "#infoIcon { color: %TEXT_FAINT%; font-size: 13px; }"
    "#infoIcon:hover { color: %ACCENT%; }"
    // 单选/复选：指示器由 TsRadioButton / TsCheckBox 手绘 —— QSS 画不出圆点与对勾，
    // 而 Fusion 的指示器固定按调色板绘制，深色卡片上是一个突兀的白方块。
    // 这里只管文字配色、给手绘指示器留出 padding-left，并把原生指示器压成 0 尺寸。
    "QRadioButton, QCheckBox { color: %TEXT2%; font-size: 12px;"
    " padding-left: 26px; spacing: 0px; }"
    "QRadioButton:hover, QCheckBox:hover { color: %TEXT%; }"
    "QRadioButton:disabled, QCheckBox:disabled { color: %TEXT_OFF%; }"
    "QRadioButton::indicator, QCheckBox::indicator { width: 0px; height: 0px; }";
// 按钮（primaryBtn / resetBtn）与文本输入框的样式统一由 ts::primaryButtonQss() /
// ts::lineEditQss() 在这些 QSS 之后追加（见 restyle()），避免与确认卡片各写一份。

// 版本号：手动维护，与 installer/TrafficScope.iss 的 MyAppVersion 保持一致
const char* kAppVersion = "1.0.0";

QLabel* hintLabel(const QString& text, int lines, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("hint"));
    l->setFixedHeight(kLabelH * lines);
    return l;
}

// ---- 历史保留天数 ----
// 用档位下拉而不是自由输入：既能避免填出天文数字把库撑爆，
// 也不必和深色主题下"QSS 画不出箭头"的 QSpinBox 较劲。
// 末尾是"永久保留"，在 ini 里写成 -1（不用 0：0 容易被理解成"保留 0 天"）
const int kRetainPresets[] = {7, 14, 30, 60, 90, 180, 365, StatsRecorder::kRetainForever};

QString retainText(int days)
{
    return days > 0 ? QStringLiteral("%1 天").arg(days) : QStringLiteral("永久保留");
}

// 常用档位 + 当前值（ini 里手改出的非档位天数也插进来，保证能正确显示）
void fillRetainDrop(ts::DropDown* c, int current)
{
    QVector<int> vals;
    for (int v : kRetainPresets)
        vals.append(v);
    if (current > 0 && !vals.contains(current))
        vals.append(current);
    std::sort(vals.begin(), vals.end(), [](int a, int b) {
        if (a < 0)
            return false;   // -1（永久保留）永远排最后
        if (b < 0)
            return true;
        return a < b;
    });
    c->clear();
    for (int v : vals)
        c->addItem(retainText(v), v);
    c->setCurrentData(current);
}

QLabel* rowLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("rowLabel"));
    l->setFixedSize(kLabelW, kRowH);
    return l;
}

// TipCard / InfoIcon：自绘悬浮提示气泡与 ⓘ 图标，已抽到共享头 src/ui/TipCard.h
// （详情弹窗的应用/域名页也用）。

// 域名维度的提示：域名是"每个域名每小时一行"，条目数远多于应用维度
QString domainRetainTip()
{
    return QStringLiteral(
        "域名记录每小时每个域名存一条，数量远多于应用记录，也更占硬盘。\n"
        "保留天数越长占得越多，磁盘空间紧张时建议设短一些。");
}

// 「未知流量」的信息气泡：说明这类流量是怎么来的（不是统计出错）
QString unknownTrafficTip()
{
    return QStringLiteral(
        "有些流量查不出属于哪个程序：程序可能已经关掉、连接记录也被清掉了，"
        "或者它走的是系统内部的网络通道，系统本身不提供归属信息。\n"
        "这类流量会单独列成「未知流量」。");
}

// 「浏览器扩展归属」的信息气泡：说清它是干嘛的、开/关各有什么后果。
// 注意气泡是纯文本 QLabel，写 **粗体** 之类的标记只会原样显示出来，别用。
QString browserExtTip()
{
    return QStringLiteral(
        "有些流量只看到一个 IP，分不清属于哪个网站。\n"
        "装上扩展，它会告诉本程序「这个域名来自哪个网站」，网站那一栏才有数据。\n"
        "程序只在开着的时候向浏览器登记这个通道，一关就撤销，不会留下后台进程。\n"
        "不装也能用，只是网站那一栏是空的，域名等其它统计照常。");
}

// 安装说明文档（extension/浏览器扩展安装步骤.txt）的定位。两种情况：
//   装好的版本  exe 就在 {app}\ ，文档在 <exe 目录>\extension\ ；
//   源码里跑    exe 在 app\build\Release\ ，往上退三级才是仓库根。
// 逐级往上找一遍，都没有就返回空串（调用方给一句兜底提示）。
QString extensionHelpDoc()
{
    const QDir exeDir(QCoreApplication::applicationDirPath());
    const QString rel = QStringLiteral("extension/浏览器扩展安装步骤.txt");
    const QStringList candidates = {
        exeDir.filePath(rel),
        exeDir.filePath(QStringLiteral("../") + rel),
        exeDir.filePath(QStringLiteral("../../") + rel),
        exeDir.filePath(QStringLiteral("../../../") + rel),
    };
    for (const QString& p : candidates) {
        if (QFileInfo::exists(p))
            return QDir::cleanPath(p);
    }
    return QString();
}

// ---- 手绘指示器 ----
// 位置写死（配合 QSS 的 padding-left: 26px 给文字留位），不依赖样式的
// subElementRect（QSS 把指示器压成 0 尺寸后那个矩形不可靠）。
const int kIndSize = 16;
const int kIndLeft = 4;

QRectF indicatorRect(const QWidget* w)
{
    return QRectF(kIndLeft, (w->height() - kIndSize) / 2.0, kIndSize, kIndSize);
}

QColor ringColor(bool hovered, bool enabled)
{
    const ts::Theme& t = ts::theme();
    if (!enabled) {
        QColor c = t.indRing;
        c.setAlpha(t.dark ? 40 : 55);
        return c;
    }
    return hovered ? t.indRingHover : t.indRing;
}

void drawRadioIndicator(QPainter& p, const QRectF& r, bool checked, bool hovered,
                        bool enabled)
{
    const ts::Theme& t = ts::theme();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = r.adjusted(0.7, 0.7, -0.7, -0.7);
    p.setPen(QPen(checked && enabled ? t.accent : ringColor(hovered, enabled), 1.4));
    p.setBrush(enabled && hovered ? t.indFillHover : t.indFill);
    p.drawEllipse(box);
    if (checked) {
        p.setPen(Qt::NoPen);
        p.setBrush(enabled ? t.accent : ts::blend(t.accent, t.dialogCard, 0.6));
        p.drawEllipse(box.adjusted(4.2, 4.2, -4.2, -4.2));
    }
}

void drawCheckIndicator(QPainter& p, const QRectF& r, bool checked, bool hovered,
                        bool enabled)
{
    const ts::Theme& t = ts::theme();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = r.adjusted(0.7, 0.7, -0.7, -0.7);
    if (checked && enabled) {
        p.setPen(Qt::NoPen);
        p.setBrush(t.accent);
        p.drawRoundedRect(box, 4.0, 4.0);
        QPen pen(t.onAccent, 1.7);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(box.left() + 3.4, box.center().y() + 0.4);
        path.lineTo(box.left() + 6.2, box.bottom() - 4.0);
        path.lineTo(box.right() - 3.2, box.top() + 4.2);
        p.drawPath(path);
        return;
    }
    p.setPen(QPen(ringColor(hovered, enabled), 1.4));
    p.setBrush(enabled && hovered ? t.indFillHover : t.indFill);
    p.drawRoundedRect(box, 4.0, 4.0);
}

// 保留原生控件的文字排版/交互（键盘、Tab、无障碍），只把指示器换成手绘
// 命中区域：QSS 把原生 ::indicator 压成 0×0（见 kSettingsQss），而 Qt 判断"点没点到按钮"
// 走的是样式给出的 indicator/click 矩形 —— 那个矩形现在是空的，于是**点手绘的小方框
// 没反应、只有点文字才生效**。这里把手绘指示器的范围补回命中区（外扩 3px 好点一点）。
bool hitsIndicator(const QWidget* w, const QPoint& pos)
{
    return indicatorRect(w).adjusted(-3, -3, 3, 3).contains(pos);
}

class TsRadioButton : public QRadioButton {
public:
    using QRadioButton::QRadioButton;

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QRadioButton::paintEvent(e);   // 先让基类画文字（指示器已被 QSS 压成 0 尺寸）
        QPainter p(this);              // 指示器最后叠上去，避免被基类的背景填充擦掉
        drawRadioIndicator(p, indicatorRect(this), isChecked(), underMouse(),
                           isEnabled());
    }
    bool hitButton(const QPoint& pos) const override
    {
        return QRadioButton::hitButton(pos) || hitsIndicator(this, pos);
    }
};

class TsCheckBox : public QCheckBox {
public:
    using QCheckBox::QCheckBox;

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QCheckBox::paintEvent(e);
        QPainter p(this);
        drawCheckIndicator(p, indicatorRect(this), isChecked(), underMouse(),
                           isEnabled());
    }
    bool hitButton(const QPoint& pos) const override
    {
        return QCheckBox::hitButton(pos) || hitsIndicator(this, pos);
    }
};

// ---- 清空历史记录的确认框 ----
// 确认框是自绘的深色卡片，与详情弹窗共用一份实现（见 ui/ConfirmCard.h）。
// 为什么不用 QMessageBox：详见 ConfirmCard.h 顶部的说明。

// 清空历史的目标维度
enum class ClearWhich {
    None,
    App,
    Domain,
};

// 弹确认框问要清哪一类。
// 注意：结果**紧跟按钮定义**给出（不要再用"1 是哪个"这种数字约定 —— 曾经因此把
// 「清空域名」映射成清应用）。按钮顺序：取消 | 清空应用记录(中间按钮) | 清空域名记录(主按钮)。
ClearWhich askClearHistory(QWidget* parent)
{
    ts::ConfirmDialog d(parent, QStringLiteral("TrafficScope - 清空历史记录"),
                    QStringLiteral("要清空哪一类历史记录？\n\n"
                                   "只删除已保存的历史记录，"
                                   "不影响当前的实时统计，删除后无法恢复。"),
                    QStringLiteral("清空域名记录"), /*withCancel=*/true,
                    QStringLiteral("清空应用记录"));
    d.exec();
    switch (d.choice()) {
    case 1:
        return ClearWhich::Domain;   // 主按钮 = 清空域名记录
    case 2:
        return ClearWhich::App;      // 中间按钮 = 清空应用记录
    default:
        return ClearWhich::None;
    }
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("TrafficScope - 设置"));
    // 无父独立窗口（不跟随浮框置顶），必须显式关掉 quit-on-close，
    // 否则关闭本弹窗会被判定为"最后一个窗口关闭"从而退出整个程序（同详情弹窗）
    setAttribute(Qt::WA_QuitOnClose, false);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    restyle();
    // 尺寸固定（不做缩放）：宽度定死，高度由最高那一页的内容决定（见 refitHeight）
    setFixedWidth(kCardW + ts::kShadowPad * 2);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(ts::kShadowPad, ts::kShadowPad,
                            ts::kShadowPad, ts::kShadowPad);
    root->setSpacing(0);
    root->addWidget(new ts::TitleBar(this, QStringLiteral("TrafficScope - 设置"),
                                     this, /*withMinimize=*/false));

    // 悬浮提示卡片：挂在弹窗上（随弹窗销毁），弹窗隐藏时由 hideEvent 收起
    m_tip = new ts::TipCard(this);

    // ---- 中栏：左侧导航 + 右侧分页 ----
    // 设置项变多之后单列会拉得很长，改成五页：导航项与页面一一对应（下标即页号）。
    // body / lay 始终指向"当前正在填充的那一页"，下面每段开头切一次即可，
    // 段内的行不用改。
    m_pages = new QStackedWidget(this);
    QVector<QWidget*> pages;
    QVector<QVBoxLayout*> lays;
    // 页序（与左侧导航 names[] 一一对应）：显示 / 统计 / 历史记录 / 网卡 / 关于
    const int kPageCount = 5;
    for (int i = 0; i < kPageCount; ++i) {
        auto* pg = new QWidget(m_pages);
        auto* pl = new QVBoxLayout(pg);
        pl->setContentsMargins(kPadX, 8, kPadX, 4);
        pl->setSpacing(0);
        m_pages->addWidget(pg);
        pages.append(pg);
        lays.append(pl);
    }

    auto* mid = new QWidget(this);
    auto* midLay = new QHBoxLayout(mid);
    midLay->setContentsMargins(0, 6, 0, 0);
    midLay->setSpacing(0);
    midLay->addWidget(buildNav(mid));
    midLay->addWidget(m_pages, 1);
    root->addWidget(mid, 1);

    // 五页内容各自成函数（见文件末尾的 buildXxxPage），构造函数只负责搭骨架
    buildNetworkPage(pages.at(3), lays.at(3));
    buildStatsPage(pages.at(1), lays.at(1));
    buildHistoryPage(pages.at(2), lays.at(2));

    buildDisplayPage(pages.at(0), lays.at(0));
    buildAboutPage(pages.at(4), lays.at(4));

    // 每页多余的高度由 stretch 吸收，内容一律顶对齐（窗口高度取最高那一页）
    for (QVBoxLayout* pl : lays)
        pl->addStretch(1);

    // ---- 底栏：应用 / 取消（没有单独的「关闭」：标题栏 ✕ 与 Esc 等同取消）----
    auto* bottom = new QWidget(this);
    bottom->setFixedHeight(kBtnH + 8);
    auto* brow = new QHBoxLayout(bottom);
    brow->setContentsMargins(kPadX, 0, kPadX, 0);
    brow->setSpacing(8);
    brow->addStretch(1);
    m_cancelBtn = new QPushButton(QStringLiteral("取消"), bottom);
    m_applyBtn = new QPushButton(QStringLiteral("应用"), bottom);
    m_applyBtn->setObjectName(QStringLiteral("primaryBtn"));
    for (QPushButton* b : {m_cancelBtn, m_applyBtn}) {
        b->setFixedSize(84, kBtnH);
        b->setCursor(Qt::PointingHandCursor);
    }
    brow->addWidget(m_cancelBtn);
    brow->addWidget(m_applyBtn);
    root->addWidget(bottom);
    root->addSpacing(6);

    connect(m_applyBtn, &QPushButton::clicked, this, &SettingsDialog::applyAll);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::close);

    // 网卡：只改暂存值（下拉可用状态跟着单选走），真正切换在「应用」时做
    connect(m_autoRadio, &QRadioButton::toggled, this, [this](bool on) {
        if (on) {
            m_devCombo->setEnabled(false);
            markDirty();
        }
    });
    connect(m_manualRadio, &QRadioButton::toggled, this, [this](bool on) {
        if (on) {
            m_devCombo->setEnabled(true);
            markDirty();
        }
    });
    m_devCombo->onActivated = [this](const QVariant&) { markDirty(); };

    // 置顶是浮框的属性：改状态走信号，由 FloatWidget 落实现（同样在「应用」时发）
    connect(m_topCheck, &QCheckBox::clicked, this, [this]() { markDirty(); });
    // 「全屏时自动隐藏」同理，也是「应用」时才真正生效
    connect(m_fsHideCheck, &QCheckBox::clicked, this, [this]() { markDirty(); });
    // 「自动贴边隐藏」同上：关掉后浮框不再吸附到屏幕边缘
    connect(m_edgeHideCheck, &QCheckBox::clicked, this, [this]() { markDirty(); });
    // 「开机自动启动」：建/删计划任务放在「应用」时做（失败要提示，见 applyAll）
    connect(m_autoStartCheck, &QCheckBox::clicked, this, [this]() { markDirty(); });
    connect(m_resetBtn, &QPushButton::clicked, this, &SettingsDialog::onResetStats);
    // 「不再统计未知流量」：勾选后抓包不再计入任何维度，历史数据不删
    connect(m_noUnattrCheck, &QCheckBox::clicked, this, [this]() { markDirty(); });
    connect(m_clearHistBtn, &QPushButton::clicked, this, &SettingsDialog::onClearHistory);
    // 输入框只改暂存值：失焦时把夹取后的结果回显，真正的生效在「应用」
    connect(m_refreshEdit, &QLineEdit::editingFinished, this, [this]() {
        bool ok = false;
        const int v = m_refreshEdit->text().trimmed().toInt(&ok);
        m_refreshEdit->setText(QString::number(
            ok ? ts::clampRefreshMs(v) : m_snap.refreshMs));
        markDirty();
    });

    // 自动模式下默认路由变化会换网卡，同步下拉框与灰字
    connect(&PacketSource::instance(), &PacketSource::deviceChanged, this,
            [this](const QString& dev) {
                if (!m_manualRadio->isChecked())
                    m_devCombo->setCurrentData(dev);
                refreshDeviceInfo();
            });

    m_infoTimer = new QTimer(this);
    m_infoTimer->setInterval(1000);
    connect(m_infoTimer, &QTimer::timeout, this, &SettingsDialog::refreshDeviceInfo);

    // 主题/字体切换：重新套 QSS 与调色板，再按新字体重新定高
    connect(&ts::themeNotifier(), &ts::ThemeNotifier::changed, this, [this]() {
        restyle();
        refitHeight();
    });

    syncDeviceChoice();
    syncFromReal();   // 其余设置项按真实状态同步一次（置顶/隐藏/自启动由浮框在显示前同步）

    // 高度按内容算好并锁死（换字体时由 refitHeight() 重算）
    refitHeight();
}

// ---------------------------------------------------------------------------
// 各页内容：body = 页面容器，lay = 该页的竖向布局（由构造函数创建后传入）
// ---------------------------------------------------------------------------

void SettingsDialog::buildDisplayPage(QWidget* body, QVBoxLayout* lay)
{
    m_topCheck = new TsCheckBox(
        QStringLiteral("窗口置顶（浮框始终显示在最前）"), body);
    m_topCheck->setFixedHeight(kRowH);
    m_topCheck->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_topCheck);

    lay->addSpacing(6);

    m_fsHideCheck = new TsCheckBox(
        QStringLiteral("全屏时自动隐藏（游戏、放映时不打扰）"), body);
    m_fsHideCheck->setFixedHeight(kRowH);
    m_fsHideCheck->setCursor(Qt::PointingHandCursor);
    m_fsHideCheck->setChecked(ts::hideOnFullscreen());
    lay->addWidget(m_fsHideCheck);

    lay->addSpacing(6);

    m_edgeHideCheck = new TsCheckBox(
        QStringLiteral("自动贴边隐藏（拖到屏幕边缘自动收起）"), body);
    m_edgeHideCheck->setFixedHeight(kRowH);
    m_edgeHideCheck->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_edgeHideCheck);

    lay->addSpacing(6);

    m_autoStartCheck = new TsCheckBox(QStringLiteral("开机自动启动"), body);
    m_autoStartCheck->setFixedHeight(kRowH);
    m_autoStartCheck->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_autoStartCheck);

    lay->addSpacing(6);
    lay->addWidget(hintLabel(
        QStringLiteral("通过计划任务在登录时启动（最高权限，不会弹 UAC）。"),
        1, body));

    lay->addSpacing(6);

    // ---- 速率单位（字节 B/s / 比特 bps）----
    auto* unitRow = new QWidget(body);
    unitRow->setFixedHeight(kRowH);
    auto* uLay = new QHBoxLayout(unitRow);
    uLay->setContentsMargins(0, 0, 0, 0);
    uLay->setSpacing(8);
    uLay->addWidget(rowLabel(QStringLiteral("速率单位"), unitRow));
    m_unitDrop = new ts::DropDown(unitRow);
    m_unitDrop->setFixedSize(150, kRowH);
    m_unitDrop->addItem(QStringLiteral("字节 B/s"), ts::kRateUnitByte);
    m_unitDrop->addItem(QStringLiteral("比特 bps"), ts::kRateUnitBit);
    m_unitDrop->setCurrentData(ts::rateUnit());
    // 选中只改暂存值：真正的切换（落盘 + 让所有界面跟着变）在「应用」时做
    m_unitDrop->onActivated = [this](const QVariant&) { markDirty(); };
    uLay->addWidget(m_unitDrop);
    uLay->addStretch(1);
    lay->addWidget(unitRow);

    lay->addSpacing(4);
    lay->addWidget(hintLabel(
        QStringLiteral("比特按 1000 进制换算，与宽带标称口径一致。"), 1, body));

    lay->addSpacing(6);

    m_peakCheck = new TsCheckBox(QStringLiteral("面板顶部显示峰值速率"), body);
    m_peakCheck->setFixedHeight(kRowH);
    m_peakCheck->setCursor(Qt::PointingHandCursor);
    m_peakCheck->setChecked(ts::showPeakRate());
    connect(m_peakCheck, &QCheckBox::clicked, this, [this]() { markDirty(); });
    lay->addWidget(m_peakCheck);

    lay->addSpacing(6);

    // ---- 界面字体（直接读系统字体列表）----
    auto* fontRow = new QWidget(body);
    fontRow->setFixedHeight(kRowH);
    auto* fLay = new QHBoxLayout(fontRow);
    fLay->setContentsMargins(0, 0, 0, 0);
    fLay->setSpacing(8);
    fLay->addWidget(rowLabel(QStringLiteral("界面字体"), fontRow));
    // 系统字体有几百项：用 ListPicker（可滚动列表 + 输入过滤），QMenu 会被折成
    // 多列铺满整屏，又丑又难找
    m_fontDrop = new ts::ListPicker(fontRow);
    m_fontDrop->setFixedSize(200, kRowH);
    for (const QString& f : QFontDatabase::families())
        m_fontDrop->addItem(f, f);
    m_fontDrop->setCurrentData(ts::theme().fontFamily);
    // 选中只改暂存值：真正的换字体（落盘 + 广播）在「应用」时做，
    // 否则取消之后字体已经被换掉了，没法还原。
    m_fontDrop->onActivated = [this](const QVariant&) { markDirty(); };
    fLay->addWidget(m_fontDrop);
    fLay->addStretch(1);
    lay->addWidget(fontRow);

    lay->addSpacing(6);

    // ---- 刷新频率（手动输入 ms）----
    auto* refreshRow = new QWidget(body);
    refreshRow->setFixedHeight(kRowH);
    auto* rLay = new QHBoxLayout(refreshRow);
    rLay->setContentsMargins(0, 0, 0, 0);
    rLay->setSpacing(8);
    rLay->addWidget(rowLabel(QStringLiteral("刷新频率"), refreshRow));
    m_refreshEdit = new QLineEdit(refreshRow);
    m_refreshEdit->setFixedSize(84, kRowH);
    m_refreshEdit->setAlignment(Qt::AlignCenter);
    // 校验器只卡"是不是数字"：范围交给 core/Settings.h 的 clampRefreshMs() 夹取。
    // 否则越界时 QLineEdit 压根不发 editingFinished，用户改了却没生效。
    m_refreshEdit->setValidator(new QIntValidator(1, 999999, m_refreshEdit));
    m_refreshEdit->setText(QString::number(ts::floatRefreshMs()));
    rLay->addWidget(m_refreshEdit);
    auto* unit = new QLabel(QStringLiteral("ms"), refreshRow);
    unit->setObjectName(QStringLiteral("rowLabel"));
    unit->setFixedHeight(kRowH);
    rLay->addWidget(unit);
    rLay->addStretch(1);
    lay->addWidget(refreshRow);

    lay->addSpacing(6);
    lay->addWidget(hintLabel(QStringLiteral("刷新越快越占用 CPU，范围 100 ~ 10000 ms。"),
                             1, body));
}

void SettingsDialog::buildStatsPage(QWidget* body, QVBoxLayout* lay)
{
    // 勾选框 + 紧跟其后的 ⓘ：说明为什么会有"查不出归属"的流量。
    // 用一行包起来，图标才能贴着文字右边的自然位置（TsCheckBox 自己画文字，
    // 塞不进额外控件）。ⓘ 同时挂在勾选框上，整行都能悬停出气泡。
    auto* unattrRow = new QWidget(body);
    unattrRow->setFixedHeight(kRowH);
    auto* unattrLay = new QHBoxLayout(unattrRow);
    unattrLay->setContentsMargins(0, 0, 0, 0);
    unattrLay->setSpacing(2);
    m_noUnattrCheck = new TsCheckBox(QStringLiteral("不再统计未知流量"), unattrRow);
    m_noUnattrCheck->setFixedHeight(kRowH);
    m_noUnattrCheck->setCursor(Qt::PointingHandCursor);
    unattrLay->addWidget(m_noUnattrCheck);
    auto* unattrTip = new ts::InfoIcon(unknownTrafficTip(),
                                       static_cast<ts::TipCard*>(m_tip), unattrRow);
    unattrTip->attachTo(m_noUnattrCheck);   // 悬停勾选框文字也弹气泡
    unattrLay->addWidget(unattrTip);
    unattrLay->addStretch(1);
    lay->addWidget(unattrRow);

    lay->addSpacing(6);
    lay->addWidget(hintLabel(
        QStringLiteral("开启后，查不出属于哪个程序的流量不再计入统计，也不再显示；\n"
                       "已保存的历史记录保留。"),
        2, body));

    lay->addSpacing(8);
    m_resetBtn = new QPushButton(QStringLiteral("重置统计"), body);
    m_resetBtn->setObjectName(QStringLiteral("resetBtn"));
    m_resetBtn->setFixedSize(96, kBtnH);
    m_resetBtn->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_resetBtn, 0, Qt::AlignLeft);

    lay->addSpacing(6);
    lay->addWidget(hintLabel(
        QStringLiteral("重置后浮框与详情里的累计流量、"
                       "速率清零；\n已保存的历史记录"
                       "不受影响。"),
        2, body));

    // ---- 浏览器扩展（可选）：「网站」维度的唯一数据来源 ----
    // 开关只决定"主程序运行期间是否向浏览器登记这个通道"（见 core/NativeHostReg.h）：
    // 关掉后不登记，扩展连不上、也不会拉起任何后台进程，「网站」维度就没有数据。
    // 下面的状态行是纯展示，连接由扩展侧发起，本机没有"重连"动作可做。
    lay->addSpacing(16);
    auto* extCaption = new QLabel(QStringLiteral("浏览器扩展（可选）"), body);
    extCaption->setObjectName(QStringLiteral("secTitle"));   // 与「关于」页标题同款
    extCaption->setFixedWidth(kContentW);                    // 显式给足宽度，别靠自适应
    extCaption->setFixedHeight(kLabelH + 2);
    lay->addWidget(extCaption);

    // 勾选框 + 紧跟其后的 ⓘ（排布口径同「不再统计未知流量」：文字与图标当一整块）
    auto* extRow = new QWidget(body);
    extRow->setFixedHeight(kRowH);
    auto* extLay = new QHBoxLayout(extRow);
    extLay->setContentsMargins(0, 0, 0, 0);
    extLay->setSpacing(2);
    m_browserExtCheck = new TsCheckBox(QStringLiteral("启用浏览器扩展归属"), extRow);
    m_browserExtCheck->setFixedHeight(kRowH);
    m_browserExtCheck->setCursor(Qt::PointingHandCursor);
    extLay->addWidget(m_browserExtCheck);
    auto* extTip = new ts::InfoIcon(browserExtTip(),
                                    static_cast<ts::TipCard*>(m_tip), extRow);
    extTip->attachTo(m_browserExtCheck);   // 悬停勾选框文字也弹气泡
    extLay->addWidget(extTip);
    extLay->addStretch(1);
    lay->addWidget(extRow);
    lay->addSpacing(2);

    m_extStatus = new QLabel(body);
    m_extStatus->setObjectName(QStringLiteral("curDev"));    // 复用网卡那行的灰字样式
    m_extStatus->setFixedWidth(kContentW);
    m_extStatus->setFixedHeight(kLabelH);
    lay->addWidget(m_extStatus);
    lay->addWidget(hintLabel(
        QStringLiteral("装上「网站」页才有数据，不装其它统计照常。\n"
                       "扩展只发域名，不看网址和参数，也不联网上传。\n"
                       "程序没开着的时候，它不在后台跑。"),
        3, body));

    // 安装步骤放在外面一个 txt 里（extension/浏览器扩展安装步骤.txt）：
    // 那几行是"照着点"的操作，塞进设置页会把这一页撑得太长，也让文案改一次就得重编。
    // 为什么不做成"一键安装"：Chrome / Edge 都不让外部程序打开扩展管理页 ——
    // 命令行传 chrome://extensions、ShellExecute/openUrl 一个都不认（也不报错，就是没反应）。
    // 加载扩展那几下只能用户自己点，所以这里只给文档。
    lay->addSpacing(8);
    auto* extHelpBtn = new QPushButton(QStringLiteral("查看安装步骤"), body);
    extHelpBtn->setFixedHeight(kBtnH);
    extHelpBtn->setCursor(Qt::PointingHandCursor);
    connect(extHelpBtn, &QPushButton::clicked, this, [this]() {
        const QString doc = extensionHelpDoc();
        if (doc.isEmpty()) {
            ts::showCardMessage(this, QStringLiteral("TrafficScope"),
                                QStringLiteral("没找到安装说明文件。\n"
                                               "它应该和扩展放在一起\n"
                                               "…\\extension\\浏览器扩展安装步骤.txt"));
            return;
        }
        // 交给系统默认程序打开（一般是记事本）
        QDesktopServices::openUrl(QUrl::fromLocalFile(doc));
    });
    lay->addWidget(extHelpBtn, 0, Qt::AlignLeft);
}

void SettingsDialog::buildHistoryPage(QWidget* body, QVBoxLayout* lay)
{
    m_appRetain = new ts::DropDown(body);
    m_domainRetain = new ts::DropDown(body);
    m_granularity = new ts::DropDown(body);
    for (ts::DropDown* c : {m_appRetain, m_domainRetain, m_granularity}) {
        c->setFixedSize(140, kRowH);
        c->setCursor(Qt::PointingHandCursor);
    }
    fillRetainDrop(m_appRetain, StatsRecorder::instance().appRetainDays());
    fillRetainDrop(m_domainRetain, StatsRecorder::instance().domainRetainDays());
    // 改完即时生效（调小时会立刻按新值清理一次过期记录）
    m_appRetain->onActivated = [this](const QVariant&) { markDirty(); };
    m_domainRetain->onActivated = [this](const QVariant&) { markDirty(); };
    // 记录粒度：小时 / 天（见 StatsDb.h 的 Granularity）。
    // 切换只影响之后落盘的数据，已有记录保持原粒度。
    m_granularity->addItem(QStringLiteral("小时"), int(Granularity::Hour));
    m_granularity->addItem(QStringLiteral("天"), int(Granularity::Day));
    m_granularity->setCurrentData(int(StatsRecorder::instance().granularity()));
    m_granularity->onActivated = [this](const QVariant&) { markDirty(); };

    auto* granRow = new QWidget(body);
    granRow->setFixedHeight(kRowH);
    auto* granLay = new QHBoxLayout(granRow);
    granLay->setContentsMargins(0, 0, 0, 0);
    granLay->setSpacing(10);
    granLay->addWidget(rowLabel(QStringLiteral("记录粒度"), granRow));
    granLay->addWidget(m_granularity);
    granLay->addStretch(1);
    lay->addWidget(granRow);

    lay->addSpacing(6);
    lay->addWidget(hintLabel(
        QStringLiteral("按天存约占按小时的 1/24，但按小时查询的精度会降低；\n"
                       "修改只影响之后新记录的数据，已有历史保持原粒度不变。"),
        2, body));

    lay->addSpacing(8);

    auto* appRow = new QWidget(body);
    appRow->setFixedHeight(kRowH);
    auto* appLay = new QHBoxLayout(appRow);
    appLay->setContentsMargins(0, 0, 0, 0);
    appLay->setSpacing(10);
    appLay->addWidget(rowLabel(QStringLiteral("应用记录"), appRow));
    appLay->addWidget(m_appRetain);
    appLay->addStretch(1);
    lay->addWidget(appRow);

    lay->addSpacing(4);

    auto* domainRow = new QWidget(body);
    domainRow->setFixedHeight(kRowH);
    auto* domainLay = new QHBoxLayout(domainRow);
    domainLay->setContentsMargins(0, 0, 0, 0);
    domainLay->setSpacing(10);

    // 「域名记录」+ 紧跟其后的 ⓘ：整块固定成标签列宽，下面的下拉框仍与"应用记录"对齐。
    // 提示同时挂在文字与图标上 —— 18px 的小图标不容易精确悬停，整段都能触发。
    auto* domainName = new QWidget(domainRow);
    domainName->setFixedWidth(kLabelW);
    auto* nameLay = new QHBoxLayout(domainName);
    nameLay->setContentsMargins(0, 0, 0, 0);
    nameLay->setSpacing(3);
    auto* nameText = new QLabel(QStringLiteral("域名记录"), domainName);
    nameText->setObjectName(QStringLiteral("rowLabel"));
    nameLay->addWidget(nameText);
    auto* tipIcon = new ts::InfoIcon(domainRetainTip(), static_cast<ts::TipCard*>(m_tip),
                                     domainName);
    tipIcon->attachTo(nameText);   // 文字也算热区
    nameLay->addWidget(tipIcon);
    nameLay->addStretch(1);

    domainLay->addWidget(domainName);
    domainLay->addWidget(m_domainRetain);
    domainLay->addStretch(1);
    lay->addWidget(domainRow);

    lay->addSpacing(8);
    lay->addWidget(hintLabel(
        QStringLiteral("超出保留期的历史记录会被自动删除。\n"
                       "历史数据保存在用户数据目录里（不在安装目录）。"),
        2, body));

    lay->addSpacing(10);
    m_clearHistBtn = new QPushButton(QStringLiteral("清空历史记录"), body);
    m_clearHistBtn->setObjectName(QStringLiteral("resetBtn"));
    m_clearHistBtn->setFixedSize(120, kBtnH);
    m_clearHistBtn->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_clearHistBtn, 0, Qt::AlignLeft);
}

void SettingsDialog::buildNetworkPage(QWidget* body, QVBoxLayout* lay)
{
    m_autoRadio = new TsRadioButton(
        QStringLiteral("自动选择网卡（跟随系统默认路由）"), body);
    m_autoRadio->setFixedHeight(kRowH);
    m_autoRadio->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_autoRadio);
    lay->addSpacing(2);

    auto* manualRow = new QWidget(body);
    manualRow->setFixedHeight(kRowH);
    auto* mrow = new QHBoxLayout(manualRow);
    mrow->setContentsMargins(0, 0, 0, 0);
    mrow->setSpacing(10);
    m_manualRadio = new TsRadioButton(QStringLiteral("手动指定"), manualRow);
    m_manualRadio->setCursor(Qt::PointingHandCursor);
    m_devCombo = new ts::DropDown(manualRow);
    mrow->addWidget(m_manualRadio);
    mrow->addWidget(m_devCombo, 1);
    lay->addWidget(manualRow);

    // 两个单选按钮的父窗口不同（一个在 body 上、一个在下拉行里），
    // 靠 QButtonGroup 保证互斥
    auto* group = new QButtonGroup(this);
    group->addButton(m_autoRadio);
    group->addButton(m_manualRadio);
    group->setExclusive(true);

    lay->addSpacing(8);
    m_curDev = new QLabel(body);
    m_curDev->setObjectName(QStringLiteral("curDev"));
    m_curDev->setFixedSize(kContentW, kLabelH);
    lay->addWidget(m_curDev);
    m_diag = new QLabel(body);
    m_diag->setObjectName(QStringLiteral("curDev"));
    m_diag->setFixedSize(kContentW, kLabelH);
    lay->addWidget(m_diag);
}

void SettingsDialog::buildAboutPage(QWidget* body, QVBoxLayout* lay)
{
    auto* aboutTitle = new QLabel(QStringLiteral("关于"), body);
    aboutTitle->setObjectName(QStringLiteral("secTitle"));
    aboutTitle->setFixedHeight(kLabelH + 2);
    lay->addWidget(aboutTitle);

    auto* verRow = new QWidget(body);
    verRow->setFixedHeight(kRowH);
    auto* vLay = new QHBoxLayout(verRow);
    vLay->setContentsMargins(0, 0, 0, 0);
    vLay->setSpacing(8);
    vLay->addWidget(rowLabel(QStringLiteral("版本"), verRow));
    auto* verVal = new QLabel(QString::fromLatin1(kAppVersion), verRow);
    verVal->setObjectName(QStringLiteral("rowLabel"));
    verVal->setFixedHeight(kRowH);
    vLay->addWidget(verVal);
    vLay->addStretch(1);
    lay->addWidget(verRow);

    // ---- 作者 ----
    auto* authorRow = new QWidget(body);
    authorRow->setFixedHeight(kRowH);
    auto* aLay = new QHBoxLayout(authorRow);
    aLay->setContentsMargins(0, 0, 0, 0);
    aLay->setSpacing(8);
    aLay->addWidget(rowLabel(QStringLiteral("作者"), authorRow));
    auto* authorVal = new QLabel(QStringLiteral("xxh"), authorRow);
    authorVal->setObjectName(QStringLiteral("rowLabel"));
    authorVal->setFixedHeight(kRowH);
    authorVal->setTextInteractionFlags(Qt::TextSelectableByMouse);
    aLay->addWidget(authorVal);
    aLay->addStretch(1);
    lay->addWidget(authorRow);

    lay->addSpacing(6);
    auto* dirTitle = new QLabel(QStringLiteral("数据目录"), body);
    dirTitle->setObjectName(QStringLiteral("rowLabel"));
    dirTitle->setFixedSize(kContentW, kRowH);
    lay->addWidget(dirTitle);

    // 完整路径不做省略：这一页就是给用户看"数据到底在哪"
    auto* dirVal = new QLabel(ts::dataDir(), body);
    dirVal->setObjectName(QStringLiteral("rowLabel"));
    dirVal->setWordWrap(true);
    dirVal->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dirVal->setFixedWidth(kContentW);
    dirVal->adjustSize();
    lay->addWidget(dirVal);

    lay->addSpacing(8);
    auto* openDirBtn = new QPushButton(QStringLiteral("打开数据目录"), body);
    openDirBtn->setFixedHeight(kBtnH);
    openDirBtn->setCursor(Qt::PointingHandCursor);
    connect(openDirBtn, &QPushButton::clicked, this, []() {
        // 目录本身不是一个"文件"，用 QDesktopServices 交给资源管理器最省事
        QDesktopServices::openUrl(QUrl::fromLocalFile(ts::dataDir()));
    });
    lay->addWidget(openDirBtn, 0, Qt::AlignLeft);

    lay->addSpacing(8);
    auto* dirHint = hintLabel(
        QStringLiteral("历史流量记录与用户设置都保存在上面这个目录，不在安装目录里；"
                       "卸载时可以选择保留或删除。"),
        3, body);
    dirHint->setWordWrap(true);
    dirHint->setFixedWidth(kContentW);
    lay->addWidget(dirHint);
}

QWidget* SettingsDialog::buildNav(QWidget* parent)
{
    auto* nav = new QWidget(parent);
    nav->setFixedWidth(kNavW);
    auto* lay = new QVBoxLayout(nav);
    lay->setContentsMargins(8, 0, 8, 0);
    lay->setSpacing(2);

    // 顺序：显示 / 统计 / 历史记录 / 网卡 / 关于（下标 = 页号，改这里要同步 m_pages 的填充顺序）
    const QString names[] = {QStringLiteral("显示"),        // 显示
                             QStringLiteral("统计"),        // 统计
                             QStringLiteral("历史记录"),
                             QStringLiteral("网卡"),        // 网卡
                             QStringLiteral("关于")};

    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);
    for (int i = 0; i < int(sizeof(names) / sizeof(names[0])); ++i) {
        auto* b = new QPushButton(names[i], nav);
        b->setObjectName(QStringLiteral("navItem"));
        b->setCheckable(true);
        b->setFixedHeight(kNavItemH);
        b->setCursor(Qt::PointingHandCursor);
        m_navGroup->addButton(b, i);
        lay->addWidget(b);
    }
    lay->addStretch(1);

    // 导航项下标 = 页号
    connect(m_navGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (m_pages && id >= 0 && id < m_pages->count())
            m_pages->setCurrentIndex(id);
    });
    m_navGroup->button(0)->setChecked(true);
    return nav;
}

SettingsDialog::State SettingsDialog::stateFromUi() const
{
    State s;
    s.topMost = m_topCheck->isChecked();
    s.hideFullscreen = m_fsHideCheck->isChecked();
    s.autoEdgeHide = m_edgeHideCheck->isChecked();
    s.autoStart = m_autoStartCheck->isChecked();
    bool ok = false;
    const int ms = m_refreshEdit->text().trimmed().toInt(&ok);
    s.refreshMs = ok ? ts::clampRefreshMs(ms) : m_snap.refreshMs;   // 输入非法按原值算
    s.font = m_fontDrop->currentData().toString();
    s.granularity = m_granularity->currentData().toInt();
    s.appRetain = m_appRetain->currentData().toInt();
    s.domainRetain = m_domainRetain->currentData().toInt();
    s.noUnattr = m_noUnattrCheck->isChecked();
    s.autoDev = m_autoRadio->isChecked();
    s.dev = m_devCombo->currentData().toString();
    s.rateUnit = m_unitDrop ? m_unitDrop->currentData().toInt() : 0;
    s.showPeak = m_peakCheck ? m_peakCheck->isChecked() : true;
    s.browserExt = m_browserExtCheck ? m_browserExtCheck->isChecked() : true;
    return s;
}

// 按真实状态刷新控件，并把这一刻的状态记进 m_snap（作为"有没有改过"的基准）。
// 每次显示都会走一遍 —— 上次没应用就关掉的改动会在这里被丢弃。
void SettingsDialog::syncFromReal()
{
    m_snap.font = ts::uiFontFamily();
    m_fontDrop->setCurrentData(m_snap.font);

    m_snap.granularity = int(StatsRecorder::instance().granularity());
    m_granularity->setCurrentData(m_snap.granularity);

    m_snap.appRetain = StatsRecorder::instance().appRetainDays();
    m_snap.domainRetain = StatsRecorder::instance().domainRetainDays();
    fillRetainDrop(m_appRetain, m_snap.appRetain);
    fillRetainDrop(m_domainRetain, m_snap.domainRetain);

    setCountUnattributed(TrafficStore::instance().countsUnattributed());
    setAutoEdgeHide(ts::autoEdgeHide());
    if (m_browserExtCheck)
        m_browserExtCheck->setChecked(ts::browserAttribution());
    if (m_unitDrop)
        m_unitDrop->setCurrentData(ts::rateUnit());
    if (m_peakCheck)
        m_peakCheck->setChecked(ts::showPeakRate());
    markDirty();
}

void SettingsDialog::markDirty()
{
    if (!m_applyBtn)
        return;
    m_applyBtn->setEnabled(stateFromUi() != m_snap);
}

// 「应用」：把暂存状态一次性写下去（只写真正改了的项）。
// 「重置统计 / 清空历史」是两个**动作**按钮，各自带二次确认、点了立即执行，
// 不走这里 —— 它们没有"暂存"一说，也不是点了应用才生效。
void SettingsDialog::applyAll()
{
    const State s = stateFromUi();
    m_refreshEdit->setText(QString::number(s.refreshMs));   // 回显夹取后的真实值

    // 网卡：失败要提示并回滚，放在最前面
    if (s.autoDev != m_snap.autoDev || s.dev != m_snap.dev) {
        m_devCombo->setEnabled(!s.autoDev);
        PacketSource& src = PacketSource::instance();
        if (s.autoDev) {
            src.setAutoSelect(true);
        } else if (!s.dev.isEmpty()
                   && (src.autoSelect() || s.dev != src.currentDevice())) {
            QString err;
            if (!src.selectDevice(s.dev, err))
                ts::showCardMessage(this, QStringLiteral("TrafficScope"),
                                QStringLiteral("切换网卡失败：\n%1")
                                    .arg(err));
        }
    }

    if (s.font != m_snap.font) {
        ts::setUiFontFamily(s.font);
        ts::setFontFamily(s.font);
    }
    if (s.refreshMs != m_snap.refreshMs)
        emit refreshMsChanged(s.refreshMs);
    if (s.topMost != m_snap.topMost)
        emit topMostToggled(s.topMost);
    if (s.hideFullscreen != m_snap.hideFullscreen)
        emit hideOnFullscreenToggled(s.hideFullscreen);
    if (s.autoEdgeHide != m_snap.autoEdgeHide)
        emit autoEdgeHideToggled(s.autoEdgeHide);
    if (s.noUnattr != m_snap.noUnattr) {
        TrafficStore::instance().setCountsUnattributed(!s.noUnattr);
        ts::setCountUnattributed(!s.noUnattr);
    }
    if (s.granularity != m_snap.granularity)
        StatsRecorder::instance().setGranularity(
            static_cast<Granularity>(s.granularity));
    if (s.appRetain != m_snap.appRetain)
        StatsRecorder::instance().setAppRetainDays(s.appRetain);
    if (s.domainRetain != m_snap.domainRetain)
        StatsRecorder::instance().setDomainRetainDays(s.domainRetain);

    if (s.rateUnit != m_snap.rateUnit) {
        ts::setRateUnit(s.rateUnit);
        setRateUnitMode(s.rateUnit);   // 立即生效：所有 formatRate() 都读这个全局开关
    }
    if (s.showPeak != m_snap.showPeak)
        ts::setShowPeakRate(s.showPeak);
    if (s.autoStart != m_snap.autoStart) {
        QString err;
        if (!ts::setAutoStart(s.autoStart, &err)) {
            QSignalBlocker block(m_autoStartCheck);
            m_autoStartCheck->setChecked(!s.autoStart);   // 回滚到真实状态
            ts::showCardMessage(this, QStringLiteral("TrafficScope"),
                            QStringLiteral("开机自启动设置失败：\n%1")
                                .arg(err));
        }
    }

    if (s.browserExt != m_snap.browserExt) {
        // 立即登记 / 注销 native host：登记存在 = "主程序在跑"，浏览器才有资格
        // 拉起 TrafficScopeHost.exe（见 core/NativeHostReg.h）。
        ts::setBrowserAttribution(s.browserExt);
        if (s.browserExt) {
            QString err;
            if (!ts::browserHostRegister(&err))
                ts::showCardMessage(this, QStringLiteral("TrafficScope"),
                                    QStringLiteral("启用浏览器扩展归属失败：\n%1").arg(err));
        } else {
            ts::browserHostUnregister();
        }
    }

    syncDeviceChoice();      // 让「当前网卡」反映切换后的真实结果
    m_snap = stateFromUi();  // 应用完，基准刷新成刚写下去的状态
    // 不自动关窗：留在设置里可以继续改别的项。基准已刷新，
    // markDirty() 会把「应用」置灰（无改动），用户点标题栏 ✕ / Esc / 「取消」才关。
    markDirty();
}

void SettingsDialog::restyle()
{
    // 按钮与输入框的样式是共用的（见 UiTheme.h），追加在页面 QSS 之后
    setStyleSheet(ts::themed(QLatin1String(kSettingsQss)) + ts::capQss()
                  + ts::primaryButtonQss() + ts::lineEditQss());
    setPalette(ts::theme().palette());
    // 手绘控件（下拉/单选/复选/气泡）不认 QSS，得自己重画
    ts::repaintAll(this);
}

void SettingsDialog::refitHeight()
{
    if (!layout() || !m_pages)   // 构造早期还没建布局，直接跳过
        return;

    // 分页后窗口高度取"最高的那一页"，切页时窗口不跳。
    // QStackedWidget 自己的 sizeHint 只反映当前页，这里显式算一遍最大值。
    int pageH = 0;
    for (int i = 0; i < m_pages->count(); ++i) {
        QLayout* pl = m_pages->widget(i)->layout();
        if (!pl)
            continue;
        pl->activate();
        pageH = qMax(pageH, pl->sizeHint().height());
    }
    m_pages->setFixedHeight(pageH);

    // 换字体后行高会变：按内容重新算一次高度（宽度是固定的）
    layout()->activate();
    setMinimumHeight(0);
    setMaximumHeight(QWIDGETSIZE_MAX);
    adjustSize();
    setFixedHeight(height());
}

void SettingsDialog::paintEvent(QPaintEvent*)
{
    // 无边框 + 透明顶层窗口：QSS 背景不渲染，卡片与外阴影必须手绘
    // （底色与详情弹窗一致，两个弹窗观感统一）
    QPainter p(this);
    ts::drawCard(p, rect(), ts::kCardRadius, ts::theme().dialogCard, ts::cBorder());
}

void SettingsDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    // 首次显示时居中到鼠标所在屏；用户拖动过就不再挪动
    if (!m_placed) {
        m_placed = true;
        QScreen* s = QGuiApplication::screenAt(QCursor::pos());
        if (!s)
            s = QGuiApplication::primaryScreen();
        if (s) {
            const QRect avail = s->availableGeometry();
            move(avail.center() - QPoint(width() / 2, height() / 2));
        }
    }
    syncDeviceChoice();          // 重开时网卡状态可能已被自动切换改变
    syncFromReal();              // 上次没应用就关掉的改动在这里被丢弃
    m_infoTimer->start();
}

void SettingsDialog::hideEvent(QHideEvent* e)
{
    m_infoTimer->stop();
    if (m_tip)
        static_cast<ts::TipCard*>(m_tip)->dismiss();   // 气泡不能留在屏幕上
    if (m_fontDrop)
        m_fontDrop->closePopup();                  // 字体弹层同理
    QDialog::hideEvent(e);
}

// 下面几个 setter 由浮框在显示前调用：它们同时是 m_snap 的来源
// —— 置顶 / 全屏隐藏 / 刷新频率 / 自启动只有浮框知道真实值。
void SettingsDialog::setTopMost(bool on)
{
    m_snap.topMost = on;
    QSignalBlocker block(m_topCheck);   // 只同步显示，不触发 markDirty
    m_topCheck->setChecked(on);
}

void SettingsDialog::setHideOnFullscreen(bool on)
{
    m_snap.hideFullscreen = on;
    QSignalBlocker block(m_fsHideCheck);
    m_fsHideCheck->setChecked(on);
}

void SettingsDialog::setAutoEdgeHide(bool on)
{
    m_snap.autoEdgeHide = on;
    QSignalBlocker block(m_edgeHideCheck);
    m_edgeHideCheck->setChecked(on);
}

void SettingsDialog::setAutoStart(bool on)
{
    m_snap.autoStart = on;
    QSignalBlocker block(m_autoStartCheck);
    m_autoStartCheck->setChecked(on);
}

void SettingsDialog::syncDeviceChoice()
{
    PacketSource& src = PacketSource::instance();
    // 网卡列表每次重建（插拔网卡后会变）；pcap 设备名存在 itemData 里
    m_devCombo->clear();
    for (const QString& d : PacketSource::availableDevices())
        m_devCombo->addItem(NetInterfacePicker::deviceDescription(d), d);
    m_devCombo->setCurrentData(src.currentDevice());

    const bool autoOn = src.autoSelect();
    m_autoRadio->setChecked(autoOn);
    m_manualRadio->setChecked(!autoOn);
    m_devCombo->setEnabled(!autoOn);
    m_snap.autoDev = autoOn;                 // 同步基准：这是"真实"的网卡选择
    m_snap.dev = src.currentDevice();

    refreshDeviceInfo();
}

void SettingsDialog::refreshDeviceInfo()
{
    PacketSource& src = PacketSource::instance();
    QString desc = src.deviceDescription();
    if (desc.isEmpty())
        desc = QStringLiteral("—");
    m_curDev->setText(m_curDev->fontMetrics().elidedText(
        QStringLiteral("当前网卡：%1").arg(desc),
        Qt::ElideMiddle, kContentW));
    m_diag->setText(QStringLiteral("抓包 %1 个 / 计入 %2 个")
                        .arg(src.packetTotal())
                        .arg(src.packetCounted()));
    refreshExtStatus();
}

// 扩展归属通道的状态：它是"有没有在收数据"，不是"装没装"——
// 装了但扩展没连上（host 没注册 / 浏览器没加载扩展）跟没装是一回事。
//
// 文案（用户指定）："N秒前收到X条数据，共计收到Q条数据"：
//   N = 距最近一次收到数据的时长（每次刷新实算），
//   X = 最后一批的条数，Q = 本次运行累计条数（大数进位到 万/亿，不写一长串）。
void SettingsDialog::refreshExtStatus()
{
    if (!m_extStatus)
        return;

    // 关掉开关时主程序根本不登记，扩展连不上是**预期状态**，别显示成"未连接（故障）"
    if (m_browserExtCheck && !m_browserExtCheck->isChecked()) {
        m_extStatus->setText(QStringLiteral("已关闭（不登记，也不会有后台进程）"));
        return;
    }

    const ts::SiteBridge& bridge = ts::SiteBridge::instance();
    const qint64 last = bridge.lastFeedMs();
    if (last <= 0) {
        m_extStatus->setText(
            QStringLiteral("未连接（未收到扩展数据）"));
        return;
    }

    const qint64 sec = (QDateTime::currentMSecsSinceEpoch() - last) / 1000;
    QString ago;
    if (sec < 5)
        ago = QStringLiteral("刚刚");
    else if (sec < 60)
        ago = QString::number(sec) + QStringLiteral("秒前");
    else if (sec < 3600)
        ago = QString::number(sec / 60) + QStringLiteral("分钟前");
    else
        ago = QString::number(sec / 3600) + QStringLiteral("小时前");

    m_extStatus->setText(
        QStringLiteral("%1收到 %2 条数据，共计收到 %3 条数据")
            .arg(ago)
            .arg(formatCount(quint64(bridge.lastBatchItems())))
            .arg(formatCount(bridge.totalItems())));
}

void SettingsDialog::setCountUnattributed(bool on)
{
    m_snap.noUnattr = !on;   // 界面上是"不再统计未知流量"
    QSignalBlocker block(m_noUnattrCheck);
    m_noUnattrCheck->setChecked(!on);
}

void SettingsDialog::setRefreshMs(int ms)
{
    m_snap.refreshMs = ts::clampRefreshMs(ms);
    m_refreshEdit->setText(QString::number(m_snap.refreshMs));
}

void SettingsDialog::onClearHistory()
{
    const ClearWhich which = askClearHistory(this);
    if (which == ClearWhich::None)
        return;
    // 清空后要 VACUUM 回收文件空间，大库会阻塞一两秒，给个等待光标
    QApplication::setOverrideCursor(Qt::WaitCursor);
    StatsRecorder::instance().clearHistory(which == ClearWhich::App,
                                           which == ClearWhich::Domain);
    QApplication::restoreOverrideCursor();
}

void SettingsDialog::onResetStats()
{
    if (ts::askConfirm(this,
                   QStringLiteral("TrafficScope - 重置统计"),
                   QStringLiteral("确定要重置统计吗？\n\n"
                                  "浮框与详情里的累计流量、"
                                  "速率会清零；\n"
                                  "已落盘的历史记录不受影响。"),
                   QStringLiteral("重置")))
        TrafficStore::instance().reset();
}
