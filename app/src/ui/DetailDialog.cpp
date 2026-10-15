#include "DetailDialog.h"

#include "ConfirmCard.h"
#include "ProcIcon.h"
#include "RangePopup.h"
#include "TipCard.h"
#include "TitleBar.h"
#include "UiTheme.h"
#include "core/NativeHost.h"
#include "core/ProcTraffic.h"
#include "core/Settings.h"
#include "core/StatsDb.h"
#include "core/SiteResolver.h"
#include "core/StatsRecorder.h"
#include "core/TrafficStore.h"

#include <algorithm>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHoverEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#  include <QStringConverter>
#endif
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace {
const int kMaxRows = 999;   // 实时页展示全部条目（不限 Top-N）
const int kHistRows = 500;  // 历史页聚合上限
const int kPidRowH = 30;    // 展开出来的 PID 明细行高（进程行是 46）
const int kNoteRowH = 26;   // 「暂无已建立的连接」这类提示行高
const int kSearchW = 180;   // 搜索框宽度（四个页面统一）
const int kSearchH = 24;    // 与「导出 CSV」按钮等高
const int kPageTopPad = 8;  // 四个页面顶部与 Tab 栏之间的留白

// 卡片内容尺寸（不含自绘标题栏 ts::kTitleH 与四周阴影留白 ts::kShadowPad）
const int kCardW = 700;
const int kCardH = 500;
// 窗口最小尺寸：保证四个表格在首列改成自适应后仍能完整显示（不出现横向滚动条）
const int kMinW = 640;
const int kMinH = 420;
// 窗口最外这一圈没有子控件覆盖，属于弹窗自身，专门用来做 resize 命中。
// 它比阴影留白 kShadowPad(8) 宽，于是贴卡片边缘向内还有几 px 也能抓到；
// body 的内边距会按两者的差值回补，保证内容尺寸不变。
const int kEdgeBand = 14;

// 无边框窗口自己实现的边缘 resize：命中位掩码
enum EdgeFlag {
    kEdgeNone = 0,
    kEdgeLeft = 1,
    kEdgeRight = 2,
    kEdgeTop = 4,
    kEdgeBottom = 8,
};

// 表头配色不再写死：底色 / 排序列文字 / 排序箭头都取当前主题
// （ts::theme().headerBg / textHover / accent），切换主题自动跟随。
const qreal kArrowSize = 7.0;
const int kHdrPad = 6;    // 表头文字与列缘的间距（与单元格数值 padding 对齐）
const int kInfoW = 14;    // 表头 ⓘ 图标的占位宽
const int kIconGap = 3;   // 文字与 ⓘ 的间距（图标挨着文字画，不挂到列的另一头）

// 自绘表头：可点击排序 + 自绘排序箭头。
// 不用 QTableWidget 自带的 setSortingEnabled：它按 item 的显示文本比较，
// "1.2GB" 会排在 "900MB" 前面；且开着排序时逐项 setItem 会错位（见项目约束）。
// 这里只负责"点击 + 画箭头"，真正的排序由 DetailDialog 在填充前对数据向量做（比数值）。
class SortHeader : public QHeaderView {
public:
    explicit SortHeader(QWidget* parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent)
    {
        setSectionsClickable(true);
        setSortIndicatorShown(false);   // 未点击任何列时不画箭头
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);   // ⓘ 悬停提示要在无按键时收到 mouseMove
    }

    // 数值列的表头右对齐：单元格里的数字是右对齐的，表头若还是左对齐，
    // 视觉上数字会像"跑到前一列下面去了"（用户反馈的"上传下载偏移"就是这个）。
    // [from, to] 之外的列（名称列、按钮列）保持左对齐。
    void setRightAligned(int from, int to)
    {
        m_rightFrom = from;
        m_rightTo = to;
    }

    // 列头 ⓘ 提示：悬停该列头的图标弹出气泡（如"本次运行以来统计"）。
    // 图标由 paintSection 自绘，不用 InfoIcon 控件；ⓘ 只管悬停，点击照样排序
    // （别吞点击：右对齐列用户常点列头右缘，吞掉会变成"点了没反应"）。
    void addInfoColumn(int col, const QString& tip, ts::TipCard* card)
    {
        m_infoCols.insert(col);
        m_infoTip = tip;
        m_tipCard = card;
    }

protected:
    // 不调用基类实现：基类把 section 画成 transparent 时会漏出表头 viewport
    // 的调色板色（Windows 下 #F0F0F0），所以整段自己画
    void paintSection(QPainter* p, const QRect& rect, int logicalIndex) const override
    {
        const QString text = model()
            ? model()->headerData(logicalIndex, Qt::Horizontal, Qt::DisplayRole).toString()
            : QString();
        const bool sorted = isSortIndicatorShown() && logicalIndex == sortIndicatorSection();

        p->save();
        // 裁剪到本列矩形：箭头/ⓘ 一旦算出界就会画进邻列，而 Qt 只重绘
        // 变化的列，越界像素清不掉，会留下"第二个排序箭头"的残影
        p->setClipRect(rect);
        p->setRenderHint(QPainter::Antialiasing, true);
        p->fillRect(rect, ts::theme().headerBg);
        p->setPen(QPen(ts::cBorder(), 1.0));   // 表头与数据行之间的分隔线
        p->drawLine(rect.bottomLeft(), rect.bottomRight());

        QFont f(ts::fontFamily());
        f.setPixelSize(11);                  // 与 QSS 的 font-size: 11px 对齐（不是 11pt）
        f.setWeight(QFont::DemiBold);        // font-weight: 600
        p->setFont(f);
        QFontMetrics fm(f);

        // 数值列右对齐：右缘与单元格数值的右缘（单元格 item padding 也是 6）对齐
        const bool right = isRightAligned(logicalIndex);
        const bool info = m_infoCols.contains(logicalIndex);
        const int textW = fm.horizontalAdvance(text);
        // 「文字 + ⓘ」整块贴齐对齐边（口径见 headBlockX）：图标一定紧跟在文字右边，
        // 不按列宽拉到另一头 —— 网站表首列很宽，图标孤零零挂在最右边会离文字老远。
        const int x = headBlockX(rect, textW, right, info);
        p->setPen(sorted ? ts::theme().textHover : ts::cTextSub());
        p->drawText(QRect(x, rect.top(), textW, rect.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, text);

        // ⓘ：紧挨文字右侧（悬停时用强调色），提示口径见 addInfoColumn
        if (info) {
            QFont mf(ts::fontFamily());
            mf.setPixelSize(12);
            p->setFont(mf);
            p->setPen(m_infoHover ? ts::theme().accent : ts::cTextFaint());
            p->drawText(QRect(infoRectFor(logicalIndex).left(), rect.top(),
                               kInfoW, rect.height()),
                        Qt::AlignCenter, QStringLiteral("ⓘ"));
        }

        if (sorted) {
            // 箭头画在这一整块的外侧：右对齐列在文字左边，左对齐列在 ⓘ 右边
            const int blockEnd = x + textW + (info ? kIconGap + kInfoW : 0);
            const qreal ax = right ? qreal(x) - 8.0 : qreal(blockEnd + 6);
            ts::drawArrow(*p, QPointF(ax, rect.center().y()), kArrowSize,
                          ts::theme().accent,
                          sortIndicatorOrder() == Qt::AscendingOrder);
        }
        p->restore();
    }

private:
    // 该列是不是右对齐（数值列）
    bool isRightAligned(int col) const
    {
        return m_rightTo >= m_rightFrom && col >= m_rightFrom && col <= m_rightTo;
    }

    // 列头里「文字 + kIconGap + ⓘ」作为一整块贴齐对齐边：右对齐列贴列右缘、
    // 左对齐列贴列左缘，所以图标永远紧跟在文字右边。列太窄时夹回左缘，
    // 宁可图标盖住文字尾部，也不把文字推出列外（出去就被裁成半个字）。
    // 返回文字起点横坐标（与传入的 rect 同一坐标系）。
    static int headBlockX(const QRect& colRect, int textW, bool right, bool info)
    {
        const int blockW = textW + (info ? kIconGap + kInfoW : 0);
        const int x = right ? colRect.right() - kHdrPad - blockW
                            : colRect.left() + kHdrPad;
        const int minX = colRect.left() + kHdrPad;
        return x < minX ? minX : x;
    }

    // ⓘ 图标在视口里的矩形（画图与判悬停必须共用这一份，否则"看得到、点不到"）
    QRect infoRectFor(int col) const
    {
        const QRect r(sectionViewportPosition(col), 0, sectionSize(col), height());
        const QString text = model()
            ? model()->headerData(col, Qt::Horizontal, Qt::DisplayRole).toString()
            : QString();
        QFont f(ts::fontFamily());
        f.setPixelSize(11);
        f.setWeight(QFont::DemiBold);
        const int textW = QFontMetrics(f).horizontalAdvance(text);
        const int x = headBlockX(r, textW, isRightAligned(col), true);
        return QRect(x + textW + kIconGap, r.top(), kInfoW, r.height());
    }

    // ⓘ 图标在视口里的热区（没有则为空矩形）
    QRect infoRectAt(const QPoint& pos) const
    {
        for (int col : m_infoCols) {
            const QRect r = infoRectFor(col);
            if (r.contains(pos))
                return r;
        }
        return QRect();
    }

    void setInfoHover(bool hover, const QRect& anchor)
    {
        if (hover == m_infoHover)
            return;
        m_infoHover = hover;
        update();   // ⓘ 变色
        if (m_tipCard) {
            if (hover && !anchor.isNull())
                m_tipCard->popup(m_infoTip,
                                 QRect(mapToGlobal(anchor.topLeft()), anchor.size()));
            else
                m_tipCard->dismiss();
        }
    }

protected:
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QRect r = infoRectAt(e->pos());
        setInfoHover(!r.isNull(), r);
        QHeaderView::mouseMoveEvent(e);
    }

    void leaveEvent(QEvent* e) override
    {
        setInfoHover(false, QRect());
        QHeaderView::leaveEvent(e);
    }

private:
    int m_rightFrom = 1;    // 默认：数值列右对齐（各表调用方显式设一遍）
    int m_rightTo = -1;
    QSet<int> m_infoCols;        // 带 ⓘ 提示的列
    QString m_infoTip;
    ts::TipCard* m_tipCard = nullptr;
    bool m_infoHover = false;
};

// 色值全部是 %TOKEN%，由 ts::themed() 按当前主题替换（见 UiTheme.h）
const char* kDialogQss =
    // 卡片背景与圆角交给 paintEvent 手绘（顶层窗口透明，QSS 的背景不渲染）
    "QDialog { background: transparent; font-family: \"%FONT%\"; }"
    // 自绘标题栏（ts::TitleBar）的样式由 ts::capQss() 额外提供
    "#totalCaption { color: %TEXT_SUB%; font-size: 12px; }"
    "#totalDown { color: %DOWN%; font-size: 13px; font-weight: 600; }"
    "#totalUp { color: %UP%; font-size: 13px; font-weight: 600; }"
    // 峰值：跟在「上传总速度」后面，用更弱一级的文字色，不抢总速度的视觉
    "#totalPeak { color: %TEXT_FAINT%; font-size: 12px; }"
    "QTabWidget::pane { border: none; background: transparent; }"
    "QTabBar::tab { background: transparent; color: %TEXT_SUB%; border: none;"
    " padding: 5px 14px; font-size: 12px; }"
    "QTabBar::tab:hover { color: %TEXT_HOVER%; }"
    "QTabBar::tab:selected { color: %TEXT%; font-weight: 600;"
    " border-bottom: 2px solid %ACCENT%; }"
    "QTableWidget { background: transparent; color: %TEXT2%; font-size: 12px;"
    " alternate-background-color: %ROW_ALT%;"
    " gridline-color: transparent; border: none; }"
    "QTableWidget::item { padding: 3px 6px; }"
    // 点过的格子（左键/右键）留一层柔和强调色；名称列是自绘 delegate，但背景同样交给样式画
    "QTableWidget::item:selected { background: %ACCENT_SOFT%; }"
    // 「已复制」轻提示：浮在卡片正中，自动消失
    "#toast { background: %POPUP_BG%; color: %TEXT%; border: 1px solid %POPUP_BORDER%;"
    " border-radius: 8px; padding: 4px 12px; font-size: 12px; }"
    // 搜索命中计数（仅关键字非空时出现）
    "#searchHint { color: %TEXT_FAINT%; font-size: 11px; }"
    // 网站页：没检测到浏览器扩展时的提示
    "#extHint { color: %TEXT_SUB%; font-size: 12px; }"
    // 表头：只给 section 设 transparent 是不够的 —— 表头自身的 viewport 仍按调色板
    // 填充（Windows 下就是 #F0F0F0），transparent 的 section 会把它漏出来，
    // 于是出现一条突兀的浅灰带。这里给表头本体与 section 都铺上卡片底色
    // （%HEADER_BG% 与 SortHeader 手绘用的 theme().headerBg 是同一个值）。
    "QHeaderView { background-color: %HEADER_BG%; }"
    "QHeaderView::section { background-color: %HEADER_BG%; color: %TEXT_SUB%;"
    " font-size: 11px; font-weight: 600; border: none;"
    " border-bottom: 1px solid %BORDER%; padding: 4px 6px; }"
    "QScrollBar:vertical { background: transparent; width: 6px; margin: 0; }"
    "QScrollBar::handle:vertical { background: %SCROLL%;"
    " border-radius: 3px; min-height: 28px; }"
    "QScrollBar::handle:vertical:hover { background: %SCROLL_HOVER%; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
    "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical"
    " { background: transparent; }"
    "QPushButton#openBtn { background: transparent; color: %TEXT_SUB%;"
    " border: 1px solid %FIELD_BORDER%; border-radius: 6px;"
    " font-size: 11px; padding: 2px 6px; }"
    "QPushButton#openBtn:hover { color: %TEXT%;"
    " border-color: %FIELD_BORDER_HOVER%; }"
    "QPushButton#openBtn:disabled { color: %TEXT_DIM%;"
    " border-color: %FIELD_BORDER_OFF%; }"
    // 「断开连接」/「结束进程」的 ✕：平时低调（不抢「打开位置」的视觉），
    // 悬停才变色 —— 这类不可撤销的操作不希望被误点。
    "QPushButton#dangerBtn { background: transparent; color: %TEXT_SUB%;"
    " border: 1px solid transparent; border-radius: 6px; font-size: 12px; }"
    "QPushButton#dangerBtn:hover { background: %ACCENT_SOFT2%; color: %DANGER%;"
    " border-color: %DANGER%; }"
    "QPushButton#dangerBtn:disabled { color: %TEXT_OFF%; }"
    "QPushButton#rangeBtn { background: transparent; color: %TEXT_SUB%;"
    " border: 1px solid %FIELD_BORDER%; border-radius: 6px;"
    " padding: 2px 10px; font-size: 12px; }"
    "QPushButton#rangeBtn:hover { color: %TEXT_HOVER%; }"
    "QPushButton#rangeBtn:checked { background: %ACCENT_SOFT%;"
    " color: %TEXT%; border-color: %ACCENT%; font-weight: 600; }"
    "QPushButton#dateBtn { background: %FIELD_BG%; color: %TEXT2%;"
    " border: 1px solid %FIELD_BORDER%; border-radius: 6px;"
    " padding: 2px 10px; font-size: 12px; }"
    "QPushButton#dateBtn:hover { color: %TEXT%;"
    " border-color: %FIELD_BORDER_HOVER%; }";

// 首列在 buildTable(..., stretchFirst=true) 里改成了 Stretch，这里的首列数值只是初值。
// 其余固定列合计 = 84*2 + 68*2 + 58(连接数) + 104(操作) = 466；视口 662（已扣 6px 垂直滚动条）
// 时首列约 196，够放进程名。操作列要同时放下「打开位置」+ ✕ 两个按钮，
// 104 是"文字不被压、✕ 不贴边"的实测下限（92 时「打开位置」会被挤变形）。
// 累计两列 84 而不是 68：要同时装下 文字(≈44) + ⓘ(16) + 排序箭头(12) + 两侧留白，
// 68 会把箭头挤出列宽（画进邻列，切列后旧箭头像素清不掉变成"两个排序标志"）。
const int kProcWidths[] = {244, 68, 68, 84, 84, 58, 104};
const int kDomainWidths[] = {380, 68, 68, 84, 84};
const int kSiteWidths[] = {380, 68, 68, 84, 84};
const int kHistWidths[] = {300, 88, 88, 88, 88};

// Tab 顺序：应用 0 / 域名 1 / 网站 2 / 历史累计 3。
// 历史页只在**可见时**才查库，别处判断"当前是不是历史页"一律用这个常量 ——
// 写成裸数字，加一个 Tab 就会静默失效（现已踩过一次：网站页插进来把索引顶到了 3）。
const int kHistTab = 3;

// stretchFirst=true 时首列（名称列）随窗口宽度自适应：窗口放大后右侧不留空、
// 缩小后也不会横向溢出（横向滚动条是关掉的，固定列宽会直接把列裁掉）
QTableWidget* buildTable(const QStringList& headers, const int* widths, int cols,
                         bool stretchFirst)
{
    auto* t = new QTableWidget(0, cols);
    t->setHorizontalHeader(new SortHeader(t));   // 自绘表头：可点击排序 + 排序箭头
    t->setHorizontalHeaderLabels(headers);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // 只做**单格**选中：点行本来就是"展开明细"，整行高亮会让人以为选中的是整行；
    // 单格高亮是给右键复制用的（告诉你复制的是哪一格），配色见 QSS ::item:selected。
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setSelectionBehavior(QAbstractItemView::SelectItems);
    t->setFocusPolicy(Qt::NoFocus);   // 不做键盘操作，免得当前格再套一层虚线焦点框
    t->setAlternatingRowColors(true);
    t->setShowGrid(false);
    t->setFrameShape(QFrame::NoFrame);
    t->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    t->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(28);
    t->horizontalHeader()->setFixedHeight(26);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    for (int i = 0; i < cols; ++i)
        t->setColumnWidth(i, widths[i]);
    if (stretchFirst)
        t->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    return t;
}

// 右对齐数值单元格（应用/域名/历史三张表共用）
void setRightCell(QTableWidget* t, int row, int col, const QString& text, const QColor& c)
{
    auto* it = new QTableWidgetItem(text);
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    it->setForeground(c);
    t->setItem(row, col, it);
}

qint64 dayStart(const QDate& d)
{
    return QDateTime(d, QTime(0, 0, 0)).toSecsSinceEpoch();
}

// ---- 排序键：数值列一律比较数值本身 ----
// 不能按显示文本比："1.2GB" 会排在 "900MB" 前面，"9.5%" 会排在 "10.1%" 前面。
bool procLess(const ProcStat& a, const ProcStat& b, int col)
{
    switch (col) {
    case 0:  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    case 1:  return a.downRate < b.downRate;
    case 2:  return a.upRate < b.upRate;
    case 3:  return a.downBytes < b.downBytes;
    case 4:  return a.upBytes < b.upBytes;
    case 5:  return a.connCount < b.connCount;
    default: return false;
    }
}

bool domainLess(const DomainStat& a, const DomainStat& b, int col)
{
    switch (col) {
    case 0:  return a.domain.compare(b.domain, Qt::CaseInsensitive) < 0;
    case 1:  return a.downRate < b.downRate;
    case 2:  return a.upRate < b.upRate;
    case 3:  return a.downBytes < b.downBytes;
    case 4:  return a.upBytes < b.upBytes;
    default: return false;
    }
}

bool siteLess(const SiteStat& a, const SiteStat& b, int col)
{
    switch (col) {
    case 0:  return a.site.compare(b.site, Qt::CaseInsensitive) < 0;
    case 1:  return a.downRate < b.downRate;
    case 2:  return a.upRate < b.upRate;
    case 3:  return a.downBytes < b.downBytes;
    case 4:  return a.upBytes < b.upBytes;
    default: return false;
    }
}

bool histLess(const StatsDb::Row& a, const StatsDb::Row& b, int col)
{
    switch (col) {
    case 0:  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    case 1:  return a.down < b.down;
    case 2:  return a.up < b.up;
    case 3:  return a.up + a.down < b.up + b.down;   // 总流量
    case 4:  return a.up + a.down < b.up + b.down;   // 占比与总流量同序
    default: return false;
    }
}

// 按当前排序状态排序：col < 0 表示还没点过表头，沿用数据自带的"总量降序"。
// 用 stable_sort，数值相同的行不会每秒抖动（实时表每秒重建一次）。
template <class Vec, class Less>
void sortRows(Vec& v, const SortState& st, Less less)
{
    if (st.col < 0)
        return;
    std::stable_sort(v.begin(), v.end(),
                     [&](const typename Vec::value_type& a, const typename Vec::value_type& b) {
                         return less(a, b, st.col);
                     });
    if (st.order == Qt::DescendingOrder)
        std::reverse(v.begin(), v.end());
}

// 关键字按空格拆词，每个词都命中才算命中（不区分大小写的子串包含）
bool matchFilter(const QString& filter, const QString& haystack)
{
    const QStringList terms = filter.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& t : terms) {
        if (!haystack.contains(t, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

// 应用页首列的自绘 delegate，负责三种行：
//   ① 进程行：展开标记 ▸/▾ + 图标 + 进程名 + 第二行 FileDescription（两行显示）；
//   ② 展开出来的 PID 明细行：「进程ID: 7392」+ 程序类别（如"第三方程序"）；
//   ③ 提示行：「暂无已建立的连接」这类灰字说明。
// 用 UserRole 区分：进程行 +1(描述)/+2(是否展开)；PID 行 +7(pid)/+8(类别)；提示行 +10。
class ProcNameDelegate : public QStyledItemDelegate {
public:
    explicit ProcNameDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    // 展开标记：未展开 → ▸（向右），已展开 → ▾（向下）。
    // 不用 ts::drawArrow：它只有上下两个方向，画不出向右的收起态。
    static void drawCaret(QPainter& p, const QPointF& c, qreal s, const QColor& col, bool down)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        QPainterPath t;
        if (down) {
            t.moveTo(c.x() - s * 0.5, c.y() - s * 0.35);
            t.lineTo(c.x() + s * 0.5, c.y() - s * 0.35);
            t.lineTo(c.x(), c.y() + s * 0.45);
        } else {
            t.moveTo(c.x() - s * 0.35, c.y() - s * 0.5);
            t.lineTo(c.x() + s * 0.45, c.y());
            t.lineTo(c.x() - s * 0.35, c.y() + s * 0.5);
        }
        t.closeSubpath();
        p.drawPath(t);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString name = opt.text;
        const QIcon icon = opt.icon;
        const QString desc = index.data(Qt::UserRole + 1).toString();
        const QVariant expandedRole = index.data(Qt::UserRole + 2);
        const QVariant noteRole = index.data(Qt::UserRole + 10);  // 有值 = 提示行

        // 背景（交替行/选中）交给样式，文字与图标手绘
        opt.text.clear();
        opt.icon = QIcon();
        QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);

        const QRect r = opt.rect.adjusted(8, 2, -8, -2);

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);

        if (noteRole.isValid()) {
            QFont nf(ts::fontFamily(), 11);
            p->setFont(nf);
            p->setPen(ts::cTextFaint());
            p->drawText(QRect(r.left() + 30, r.top(), r.width() - 30, r.height()),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(nf).elidedText(opt.text, Qt::ElideRight, r.width() - 30));
            p->restore();
            return;
        }
        if (index.data(Qt::UserRole + 7).isValid()) {
            drawPidRow(p, r, index);
            p->restore();
            return;
        }

        int textX = r.left();

        // 展开标记：往右（未展开）/ 往下（已展开），画在最左侧
        if (expandedRole.isValid()) {
            const bool expanded = expandedRole.toBool();
            drawCaret(*p, QPointF(r.left() + 4.0, r.center().y()), kArrowSize * 1.1,
                      expanded ? ts::theme().accent : ts::cTextFaint(), expanded);
            textX += 14;
        }
        if (!icon.isNull()) {
            const int s = 18;
            const QRect ir(textX, r.top() + (r.height() - s) / 2, s, s);
            icon.paint(p, ir, Qt::AlignCenter, QIcon::Normal, QIcon::On);
            textX += s + 8;
        }
        const int textW = r.right() - textX;
        if (textW <= 0) {
            p->restore();
            return;
        }

        QFont f(ts::fontFamily(), 12);
        QFontMetrics fm(f);
        QFont df(ts::fontFamily(), 10);
        QFontMetrics dfm(df);

        // 两行整体垂直居中，中间留 kLineGap：原实现两行只隔 1px 且贴着行高边缘，
        // 所以显得紧凑（行高已同步由 38 调到 46）
        const int nameH = fm.height();
        const int descH = dfm.height();
        const int kLineGap = 4;
        const int block = desc.isEmpty() ? nameH : nameH + kLineGap + descH;
        const int top = r.top() + qMax(0, (r.height() - block) / 2);

        p->setFont(f);
        p->setPen(ts::cText());
        p->drawText(QRect(textX, top, textW, nameH), Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(name, Qt::ElideRight, textW));

        if (!desc.isEmpty()) {
            p->setFont(df);
            p->setPen(ts::blend(ts::cTextFaint(), ts::cTextSub(), 0.5));   // 比 faint 略亮，更好读
            p->drawText(QRect(textX, top + nameH + kLineGap, textW, descH),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        dfm.elidedText(desc, Qt::ElideRight, textW));
        }
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override
    {
        // 子行更紧凑一些（setRowHeight 已同步设过，这里保持一致）
        if (index.data(Qt::UserRole + 10).isValid())
            return QSize(0, kNoteRowH);
        if (index.data(Qt::UserRole + 7).isValid())
            return QSize(0, kPidRowH);
        return QSize(0, 46);
    }

private:
    // PID 明细行：「进程ID: 7392」 + 程序类别（系统程序 / 第三方程序）
    static void drawPidRow(QPainter* p, const QRect& r, const QModelIndex& index)
    {
        const QString head = QStringLiteral("进程ID: %1").arg(index.data(Qt::UserRole + 7).toULongLong());
        const QString cat = index.data(Qt::UserRole + 8).toString();

        const int x0 = r.left() + 14;   // 与进程行的展开标记同一起点

        QFont f(ts::fontFamily(), 12);
        p->setFont(f);
        QFontMetrics fm(f);
        const int textX = x0 + 16;
        const int headW = fm.horizontalAdvance(head);
        p->setPen(ts::theme().text2);
        p->drawText(QRect(textX, r.top(), headW, r.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, head);

        if (!cat.isEmpty()) {
            QFont cf(ts::fontFamily(), 11);
            QFontMetrics cfm(cf);
            const int cx = textX + headW + 12;
            const int cw = r.right() - cx;
            if (cw > 24) {
                p->setFont(cf);
                p->setPen(ts::blend(ts::cTextFaint(), ts::cTextSub(), 0.5));
                p->drawText(QRect(cx, r.top(), cw, r.height()),
                            Qt::AlignLeft | Qt::AlignVCenter,
                            cfm.elidedText(cat, Qt::ElideRight, cw));
            }
        }
    }

};
}  // namespace

DetailDialog::DetailDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("TrafficScope - 流量监控"));
    // 无父独立窗口（不跟随浮框置顶），默认带 WA_QuitOnClose；
    // 而浮框是 Qt::Tool，Qt 不会给 Tool 窗口设该属性 —— 关闭本弹窗会被判定为
    // "最后一个 quit-on-close 窗口关闭" 从而把整个程序退出。这里显式关掉。
    // 退出仍走浮框右键菜单里的 QCoreApplication::quit()，不受影响。
    setAttribute(Qt::WA_QuitOnClose, false);
    // 自绘标题栏：去掉系统标题栏，整个窗口是"圆角深色卡片 + 柔和外阴影"
    // （Win10 不支持给原生标题栏自定义颜色，只有 Win11 22000+ 支持，自绘才能跨版本一致）
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    // 初始窗口 = 卡片 700x500 + 自绘标题栏 36 + 四周阴影留白 8x2 = 716x552；
    // 恢复上次调整过的大小（程序根目录 settings.ini 的 Dialog/size）
    setMinimumSize(kMinW, kMinH);
    QSize initSize(kCardW + ts::kShadowPad * 2,
                   kCardH + ts::kTitleH + ts::kShadowPad * 2);
    const QSize savedSize = ts::appSettings().value(QStringLiteral("Dialog/size")).toSize();
    if (savedSize.isValid() && savedSize.width() >= kMinW && savedSize.height() >= kMinH) {
        const QScreen* scr = QGuiApplication::primaryScreen();
        const QRect avail = scr ? scr->availableGeometry() : QRect();
        // 换到更小的屏幕时不要把窗口撑出可用区域
        initSize = avail.isValid()
            ? QSize(qBound(kMinW, savedSize.width(), avail.width()),
                    qBound(kMinH, savedSize.height(), avail.height()))
            : savedSize;
    }
    resize(initSize);
    restyle();
    // 无按键移动鼠标时也要能刷新边缘 resize 光标；WA_Hover 让鼠标移到表格等
    // 子控件上方时也能（沿祖先链）收到 hover 事件，避免 ⤡ 光标残留在表格上
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);

    // 四周留出 kEdgeBand 供窗口自己做 resize 命中（这一圈内没有子控件）
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kEdgeBand, kEdgeBand, kEdgeBand, kEdgeBand);
    root->setSpacing(0);
    root->addWidget(new ts::TitleBar(
        this, QStringLiteral("TrafficScope - 流量监控"), this));

    // 卡片内主体：内边距按 kEdgeBand 与 kShadowPad 的差值回补，
    // 保证表格视口宽度（668）与内容高度和原来完全一致
    auto* body = new QWidget;
    auto* bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(10, 6, 10, 6);
    bodyLay->setSpacing(10);
    root->addWidget(body, 1);

    // ---- 总速率 ----
    auto* totals = new QWidget;
    totals->setFixedHeight(22);
    auto* tLay = new QHBoxLayout(totals);
    tLay->setContentsMargins(0, 0, 0, 0);
    tLay->setSpacing(8);
    auto* downCap = new QLabel(QStringLiteral("下载总速度："));
    downCap->setObjectName(QStringLiteral("totalCaption"));
    m_totalDown = new QLabel(QStringLiteral("↓ 0B/s"));
    m_totalDown->setObjectName(QStringLiteral("totalDown"));
    auto* upCap = new QLabel(QStringLiteral("上传总速度："));
    upCap->setObjectName(QStringLiteral("totalCaption"));
    m_totalUp = new QLabel(QStringLiteral("↑ 0B/s"));
    m_totalUp->setObjectName(QStringLiteral("totalUp"));
    m_peakLabel = new QLabel(totals);
    m_peakLabel->setObjectName(QStringLiteral("totalPeak"));
    m_peakLabel->setFixedHeight(22);
    tLay->addWidget(downCap);
    tLay->addWidget(m_totalDown);
    tLay->addSpacing(24);
    tLay->addWidget(upCap);
    tLay->addWidget(m_totalUp);
    tLay->addWidget(m_peakLabel);
    tLay->addStretch();
    bodyLay->addWidget(totals);

    // ---- 实时速率曲线（最近 120 拍 = 约 2 分钟，随刷新周期变化）----
    m_graph = new ts::SpeedGraph(body);
    bodyLay->addWidget(m_graph);

    // ---- 分页：应用 / 域名 / 历史累计 ----
    // ⓘ 图标的悬浮气泡：挂在弹窗上（随弹窗销毁），弹窗收起时由 hideEvent 收起
    m_tipCard = new ts::TipCard(this);
    m_tabs = new QTabWidget;
    m_procTable = buildTable(
        {QStringLiteral("程序名称"),
         QStringLiteral("下载速度"), QStringLiteral("上传速度"),
         QStringLiteral("累计下载"), QStringLiteral("累计上传"),
         QStringLiteral("连接数"), QStringLiteral("操作")},
        kProcWidths, 7, true);
    m_procTable->verticalHeader()->setDefaultSectionSize(46);   // 两行（名称 + 描述），留出行距
    m_procTable->setItemDelegateForColumn(0, new ProcNameDelegate(m_procTable));
    // 数值列表头右对齐，与右对齐的数值对齐（否则数字看着像跑到前一列下面）
    static_cast<SortHeader*>(m_procTable->horizontalHeader())->setRightAligned(1, 5);
    m_domainTable = buildTable(
        {QStringLiteral("域名"),
         QStringLiteral("下载速度"), QStringLiteral("上传速度"),
         QStringLiteral("累计下载"), QStringLiteral("累计上传")},
        kDomainWidths, 5, true);
    static_cast<SortHeader*>(m_domainTable->horizontalHeader())->setRightAligned(1, 4);
    m_siteTable = buildTable(
        {QStringLiteral("网站"),
         QStringLiteral("下载速度"), QStringLiteral("上传速度"),
         QStringLiteral("累计下载"), QStringLiteral("累计上传")},
        kSiteWidths, 5, true);
    static_cast<SortHeader*>(m_siteTable->horizontalHeader())->setRightAligned(1, 4);
    // 网站列头加 ⓘ：说明"网站"是什么、以及 * 的含义。
    // 一个 SortHeader 只有一份 tip 文案，所以这张表只在名称列加，不跟累计列混用。
    static_cast<SortHeader*>(m_siteTable->horizontalHeader())
        ->addInfoColumn(0,
                        QStringLiteral("网站 = 域名的注册域名，"
                                       "并把 DNS CNAME 链上的 CDN 域名"
                                       "归到主站；* 表示含被多"
                                       "个网站共用的 CDN，归属可"
                                       "能不准"),
                        m_tipCard);
    connectSort(m_procTable, m_procSort);
    connectSort(m_domainTable, m_domainSort);
    connectSort(m_siteTable, m_siteSort);
    // 点网站行 → 展开组成它的域名 / IP
    connect(m_siteTable, &QTableWidget::itemClicked, this, &DetailDialog::onSiteRowClicked);
    // 「累计下载 / 累计上传」列头加 ⓘ：提示这两列是本次运行的内存实时累计
    // （重启清零），与「历史累计」页的落库数字不同源。
    const QString cumTip = QStringLiteral("本次运行以来统计");
    auto* procHdr = static_cast<SortHeader*>(m_procTable->horizontalHeader());
    procHdr->addInfoColumn(3, cumTip, m_tipCard);
    procHdr->addInfoColumn(4, cumTip, m_tipCard);
    auto* domHdr = static_cast<SortHeader*>(m_domainTable->horizontalHeader());
    domHdr->addInfoColumn(3, cumTip, m_tipCard);
    domHdr->addInfoColumn(4, cumTip, m_tipCard);
    // 点进程行 → 展开该进程的 PID 明细；点 PID 行 → 再展开它的连接。
    // 「操作」列是 cellWidget（按钮），不会走到这里。
    connect(m_procTable, &QTableWidget::itemClicked,
            this, &DetailDialog::onProcRowClicked);
    // 表格外面包一层页面容器：左上放搜索框，右上放「导出 CSV」按钮
    auto makeTablePage = [this](QTableWidget* table, const QString& fileTag,
                                SearchBox& box, const QString& placeholder,
                                QLabel* midHint = nullptr) {
        auto* page = new QWidget;
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, kPageTopPad, 0, 0);
        lay->setSpacing(6);
        auto* top = new QWidget(page);
        auto* topLay = new QHBoxLayout(top);
        topLay->setContentsMargins(0, 0, 0, 0);
        topLay->setSpacing(8);
        addSearchBox(top, topLay, box, placeholder, false);
        topLay->addStretch(1);
        if (midHint) {
            midHint->setParent(top);
            topLay->addWidget(midHint);
            topLay->addStretch(1);
        }
        auto* btn = new QPushButton(QStringLiteral("导出 CSV"), top);
        btn->setObjectName(QStringLiteral("rangeBtn"));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(24);
        connect(btn, &QPushButton::clicked, this,
                [this, table, fileTag]() { exportTable(table, fileTag); });
        topLay->addWidget(btn);
        lay->addWidget(top);
        lay->addWidget(table, 1);
        return page;
    };

    m_tabs->addTab(makeTablePage(m_procTable, QStringLiteral("apps"), m_procSearch,
                                 QStringLiteral("搜索程序名 / 进程ID")),
                     QStringLiteral("应用"));
    m_tabs->addTab(makeTablePage(m_domainTable, QStringLiteral("domains"), m_domainSearch,
                                 QStringLiteral("搜索域名")),
                     QStringLiteral("域名"));
    m_siteExtHint = new QLabel(
        QStringLiteral("未检测到浏览器扩展，网站数据为空（安装方法见「设置 → 统计 → 浏览器扩展」）"));
    m_siteExtHint->setObjectName(QStringLiteral("extHint"));
    m_siteExtHint->hide();
    m_tabs->addTab(makeTablePage(m_siteTable, QStringLiteral("sites"), m_siteSearch,
                                 QStringLiteral("搜索网站 / 组成域名"), m_siteExtHint),
                   QStringLiteral("网站"));
    m_tabs->addTab(makeHistoryPage(), QStringLiteral("历史累计"));
    bodyLay->addWidget(m_tabs, 1);

    // 四张表都支持"右键单元格 → 复制"（含历史页）
    for (QTableWidget* t : {m_procTable, m_domainTable, m_siteTable, m_histTable})
        attachCopyMenu(t);

    // 「已复制」轻提示：浮在卡片正中，不进布局（进出布局会把表格挤动）
    m_toast = new QLabel(this);
    m_toast->setObjectName(QStringLiteral("toast"));
    m_toast->setAttribute(Qt::WA_TransparentForMouseEvents, true);   // 别挡住右键/悬停
    m_toast->hide();
    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    m_toastTimer->setInterval(1200);
    connect(m_toastTimer, &QTimer::timeout, m_toast, &QWidget::hide);

    // 跟随用户在设置里配的刷新周期（与浮框同一个 [Float] refreshMs，见 core/Settings.h）
    m_timer = new QTimer(this);
    m_timer->setInterval(ts::floatRefreshMs());
    // 槽带了默认参数就不能直接接无参信号（Qt 要求槽的形参不多于信号），
    // 用 lambda 走同一个入口：只有定时节拍才喂曲线
    connect(m_timer, &QTimer::timeout, this, [this]() { refresh(); });
    m_timer->start();
    // 主题切换：重新套一遍 QSS 与调色板
    connect(&ts::themeNotifier(), &ts::ThemeNotifier::changed,
            this, &DetailDialog::restyle);
    refresh();
    setHistoryRange(0);   // 默认今日
    setHistoryDim(0);     // 默认应用维度
}

QWidget* DetailDialog::makeHistoryPage()
{
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, kPageTopPad, 0, 0);
    lay->setSpacing(8);

    m_popup = new RangePopup(this);
    const QDate today = QDate::currentDate();
    const qint64 first = StatsDb::instance().earliestTs();
    m_customFrom = first > 0 ? QDateTime::fromSecsSinceEpoch(first).date() : today;
    m_customTo = today;

    m_fromBtn = new QPushButton(m_customFrom.toString(QStringLiteral("yyyy-MM-dd")));
    m_toBtn = new QPushButton(m_customTo.toString(QStringLiteral("yyyy-MM-dd")));
    for (QPushButton* b : {m_fromBtn, m_toBtn}) {
        b->setObjectName(QStringLiteral("dateBtn"));
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedHeight(24);
    }
    connect(m_fromBtn, &QPushButton::clicked, this, [this]() {
        m_pickingFrom = true;
        m_popup->popupFor(m_fromBtn, m_customFrom);
    });
    connect(m_toBtn, &QPushButton::clicked, this, [this]() {
        m_pickingFrom = false;
        m_popup->popupFor(m_toBtn, m_customTo);
    });
    connect(m_popup, &RangePopup::datePicked, this, &DetailDialog::onRangePicked);
    connect(m_popup, &RangePopup::quickRange, this, [this](int days) {
        setHistoryRange(days);
    });

    // 行 1：搜索框（左） + 时间区间（右），深色卡片样式
    auto* row1 = new QWidget;
    row1->setFixedHeight(26);
    auto* r1 = new QHBoxLayout(row1);
    r1->setContentsMargins(0, 0, 0, 0);
    r1->setSpacing(8);
    addSearchBox(row1, r1, m_histSearch, QStringLiteral("搜索名称"), true);
    r1->addStretch(1);
    auto* timeCap = new QLabel(QStringLiteral("时间："));
    timeCap->setObjectName(QStringLiteral("totalCaption"));
    auto* wave = new QLabel(QStringLiteral(" — "));
    wave->setObjectName(QStringLiteral("totalCaption"));
    r1->addWidget(timeCap);
    r1->addWidget(m_fromBtn);
    r1->addWidget(wave);
    r1->addWidget(m_toBtn);
    r1->addSpacing(10);
    auto* histExport = new QPushButton(QStringLiteral("导出 CSV"), row1);
    histExport->setObjectName(QStringLiteral("rangeBtn"));
    histExport->setCursor(Qt::PointingHandCursor);
    histExport->setFixedHeight(24);
    connect(histExport, &QPushButton::clicked, this,
            [this, histExport]() { exportTable(m_histTable, QStringLiteral("history")); });
    r1->addWidget(histExport);
    lay->addWidget(row1);

    // 行 2：区间总量（左） + 区间摘要（右）
    auto* row2 = new QWidget;
    row2->setFixedHeight(20);
    auto* r2 = new QHBoxLayout(row2);
    r2->setContentsMargins(0, 0, 0, 0);
    r2->setSpacing(8);
    auto* downCap = new QLabel(QStringLiteral("下载总量："));
    downCap->setObjectName(QStringLiteral("totalCaption"));
    m_histDown = new QLabel(QStringLiteral("0B"));
    m_histDown->setObjectName(QStringLiteral("totalDown"));
    auto* upCap = new QLabel(QStringLiteral("上传总量："));
    upCap->setObjectName(QStringLiteral("totalCaption"));
    m_histUp = new QLabel(QStringLiteral("0B"));
    m_histUp->setObjectName(QStringLiteral("totalUp"));
    r2->addWidget(downCap);
    r2->addWidget(m_histDown);
    r2->addSpacing(16);
    r2->addWidget(upCap);
    r2->addWidget(m_histUp);
    r2->addStretch(1);
    m_histSummary = new QLabel;
    m_histSummary->setObjectName(QStringLiteral("totalCaption"));
    r2->addWidget(m_histSummary);
    lay->addWidget(row2);

    // 维度切换放 Tab 栏右侧角标（仅历史页可见）
    m_dimCorner = new QWidget;
    m_dimCorner->setFixedHeight(26);
    auto* cl = new QHBoxLayout(m_dimCorner);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(4);
    m_histAppBtn = new QPushButton(QStringLiteral("应用"));
    m_histDomainBtn = new QPushButton(QStringLiteral("域名"));
    m_histSiteBtn = new QPushButton(QStringLiteral("网站"));
    for (QPushButton* b : {m_histAppBtn, m_histDomainBtn, m_histSiteBtn}) {
        b->setObjectName(QStringLiteral("rangeBtn"));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedHeight(24);
        cl->addWidget(b);
    }
    connect(m_histAppBtn, &QPushButton::clicked, this, [this]() { setHistoryDim(0); });
    connect(m_histDomainBtn, &QPushButton::clicked, this, [this]() { setHistoryDim(1); });
    connect(m_histSiteBtn, &QPushButton::clicked, this, [this]() { setHistoryDim(2); });
    m_tabs->setCornerWidget(m_dimCorner, Qt::TopRightCorner);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int i) {
        m_dimCorner->setVisible(i == kHistTab);
        if (i == kHistTab)
            refreshHistory();   // 切到历史页立即查，避免停留在打开弹窗时的旧快照
    });
    m_dimCorner->setVisible(false);

    // 历史趋势柱状图：区间越粗，桶越大（<= 2 天按小时，更长按天）
    m_histChart = new ts::HistoryChart(page);
    lay->addWidget(m_histChart);

    m_histTable = buildTable(
        {QStringLiteral("名称"), QStringLiteral("累计下载"),
         QStringLiteral("累计上传"), QStringLiteral("总流量"),
         QStringLiteral("占比")},
        kHistWidths, 5, true);
    static_cast<SortHeader*>(m_histTable->horizontalHeader())->setRightAligned(1, 4);
    connectSort(m_histTable, m_histSort);
    lay->addWidget(m_histTable, 1);
    return page;
}

QIcon DetailDialog::iconFor(const ProcStat& s)
{
    return ts::procIcon(s.name, s.pid, m_iconCache);
}

namespace {
// 「操作」列：把内容塞进一个居中容器，避免直接放按钮时贴边
QWidget* centeredButton(QPushButton* btn)
{
    auto* box = new QWidget;
    auto* lay = new QHBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    lay->addStretch(1);
    lay->addWidget(btn);
    lay->addStretch(1);
    return box;
}

// 不可撤销的操作（断开连接 / 结束进程）统一用这个样式的 ✕
QPushButton* dangerButton(const QString& what)
{
    auto* b = new QPushButton(QStringLiteral("✕"));
    b->setObjectName(QStringLiteral("dangerBtn"));
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedSize(22, 20);
    b->setToolTip(what);
    return b;
}


}  // namespace

void DetailDialog::refresh(bool sampleCurve)
{
    TrafficStore& store = TrafficStore::instance();
    m_current = store.procSnapshot(kMaxRows);
    QVector<DomainStat> domains = store.snapshot(kMaxRows, 0);
    // 网站维度与域名快照同一拍（内部有 500ms 缓存），不会多推进一次差分基准
    m_sites = store.siteSnapshot(kMaxRows);
    updateSiteExtHint();

    // 按表头点过的列排序；未点过（col<0）时沿用数据自带的"总量降序"
    sortRows(m_current, m_procSort, procLess);
    sortRows(m_sites, m_siteSort, siteLess);
    sortRows(domains, m_domainSort, domainLess);

    quint64 up = 0;
    quint64 down = 0;
    store.totals(up, down);
    m_totalDown->setText(QStringLiteral("↓ %1").arg(formatRate(down)));
    m_totalUp->setText(QStringLiteral("↑ %1").arg(formatRate(up)));

    // 峰值速率跟在「上传总速度」后面：曲线本身是自适应缩放的，
    // 峰值放进图里会把纵轴钉死在历史最高点（突发流量一过两条线就贴地）。
    if (ts::showPeakRate()) {
        quint64 peakUp = 0;
        quint64 peakDown = 0;
        store.peaks(peakUp, peakDown);
        m_peakLabel->show();
        m_peakLabel->setText(
            QStringLiteral("   ·   峰值 ↓%1 ↑%2")
                .arg(formatRate(peakDown), formatRate(peakUp)));
    } else {
        m_peakLabel->hide();
    }

    // 喂一拍样本给顶部曲线（刷新周期改了，曲线覆盖的时间跨度也会跟着变）。
    // 只有定时节拍喂：交互触发的重建若也喂，搜索时每敲一个字曲线就多一个点，
    // 横向时间轴被挤乱，看起来就像"曲线跟着搜索框变"
    if (m_graph && sampleCurve)
        m_graph->push(up, down);

    // ---- 应用页：先排眼前的行布局（进程行 + 展开出来的 PID 明细）----
    // 搜索口径：进程行匹配「程序名 + 描述 + 进程ID」，主行未命中但名下 PID 命中的也列出
    const QString procFilter = m_procSearch.filter();
    m_plan.clear();
    int procShown = 0;
    for (int i = 0; i < m_current.size(); ++i) {
        const ProcStat& s = m_current.at(i);
        const bool selfHit = matchFilter(
            procFilter, s.name + QLatin1Char(' ') + s.desc + QLatin1Char(' ')
                            + QString::number(s.pid));
        const bool expanded = m_expanded.contains(s.name);

        // 要显示的 PID 明细：名下有已建立连接的那些（连接数为 0 的不列）
        QVector<ProcPidStat> pids;
        if (expanded || !selfHit) {
            const QVector<ProcPidStat> all = store.procDetail(s.name);
            for (const ProcPidStat& ps : all) {
                if (ps.connCount <= 0)
                    continue;
                if (selfHit || matchFilter(procFilter, QString::number(ps.pid)))
                    pids.append(ps);
            }
        }
        if (!selfHit && pids.isEmpty())
            continue;   // 主行与明细都没命中

        // 展开态：用户展开，或靠 PID 搜出明细
        const bool open = expanded || !selfHit;

        RowPlan row;
        row.kind = RowPlan::Proc;
        row.procIdx = i;
        row.open = open;
        m_plan.append(row);
        ++procShown;

        if (!open)
            continue;

        for (const ProcPidStat& ps : pids) {
            RowPlan pr;
            pr.kind = RowPlan::Pid;
            pr.procIdx = i;
            pr.pid = ps.pid;
            pr.pidStat = ps;
            m_plan.append(pr);
        }
        // 真的一个已建立连接都没有时才提示"暂无"
        if (pids.isEmpty() && selfHit) {
            RowPlan note;
            note.kind = RowPlan::Note;
            note.procIdx = i;
            m_plan.append(note);
        }
    }

    m_procTable->setRowCount(m_plan.size());
    // 每次重建都要先清 span：行索引会随排序变化，若不清理，
    // 上一拍是"连接子行"的位置这一拍变成进程行时，残留的合并会盖掉后面的列。
    m_procTable->clearSpans();
    for (int row = 0; row < m_plan.size(); ++row) {
        const RowPlan& p = m_plan.at(row);
        switch (p.kind) {
        case RowPlan::Proc:
            fillProcRow(row, m_current.at(p.procIdx), p.open);
            break;
        case RowPlan::Pid:
            fillPidRow(row, p);
            break;
        case RowPlan::Note:
            fillNoteRow(row, QStringLiteral("暂无已建立的连接"));
            break;
        }
    }
    updateSearchHint(m_procSearch, procShown, m_current.size());

    // ---- 域名页 ----
    // 只放命中的行，末尾多余的行裁掉
    const QString domainFilter = m_domainSearch.filter();
    m_domainTable->setRowCount(domains.size());
    int domainRow = 0;
    for (int i = 0; i < domains.size(); ++i) {
        const DomainStat& s = domains.at(i);
        if (!matchFilter(domainFilter, s.domain))
            continue;

        auto* name = new QTableWidgetItem(s.domain);
        name->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        name->setForeground(ts::cText());
        m_domainTable->setItem(domainRow, 0, name);

        setRightCell(m_domainTable, domainRow, 1, formatRate(s.downRate), ts::cDown());
        setRightCell(m_domainTable, domainRow, 2, formatRate(s.upRate), ts::cUp());
        setRightCell(m_domainTable, domainRow, 3, formatBytes(s.downBytes), ts::cTextSub());
        setRightCell(m_domainTable, domainRow, 4, formatBytes(s.upBytes), ts::cTextSub());
        ++domainRow;
    }
    m_domainTable->setRowCount(domainRow);
    updateSearchHint(m_domainSearch, domainRow, domains.size());

    // ---- 网站页：网站行 + 展开出来的域名 / IP 明细行 ----
    // 搜索口径：网站名命中列整行，组成域名命中只列该域名
    const QString siteFilter = m_siteSearch.filter();
    m_sitePlan.clear();
    int siteShown = 0;
    for (int i = 0; i < m_sites.size(); ++i) {
        const SiteStat& st = m_sites.at(i);
        const bool selfHit = matchFilter(siteFilter, st.site);
        const bool expanded = m_siteExpanded.contains(st.site);

        QVector<SiteMember> members;
        if (expanded || !selfHit) {
            const QVector<SiteMember> all = store.siteMembers(st.site);
            for (const SiteMember& m : all) {
                if (selfHit || matchFilter(siteFilter, m.name))
                    members.append(m);
            }
        }
        if (!selfHit && members.isEmpty())
            continue;

        const bool open = expanded || !selfHit;
        SiteRow row;
        row.kind = SiteRow::Site;
        row.siteIdx = i;
        row.open = open;
        m_sitePlan.append(row);
        ++siteShown;

        if (!open)
            continue;
        for (const SiteMember& m : members) {
            SiteRow mr;
            mr.kind = SiteRow::Member;
            mr.siteIdx = i;
            mr.member = m;
            m_sitePlan.append(mr);
        }
    }

    m_siteTable->clearSpans();
    m_siteTable->setRowCount(m_sitePlan.size());
    for (int row = 0; row < m_sitePlan.size(); ++row) {
        const SiteRow& r = m_sitePlan.at(row);
        if (r.kind == SiteRow::Site)
            fillSiteRow(row, m_sites.at(r.siteIdx), r.open);
        else
            fillSiteMemberRow(row, r.member);
    }
    updateSearchHint(m_siteSearch, siteShown, m_sites.size());

    // 历史页每 10s 同步一次，且仅在历史页可见时查库（避免无谓 SQL）
    if (m_tabs->currentIndex() == kHistTab && ++m_histTick >= 10) {
        m_histTick = 0;
        refreshHistory();
    }
}

void DetailDialog::fillProcRow(int row, const ProcStat& s, bool expanded)
{
    m_procTable->setRowHeight(row, 46);

    auto* name = new QTableWidgetItem(s.name);
    name->setIcon(iconFor(s));
    name->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    name->setForeground(ts::cText());
    name->setData(Qt::UserRole + 1, s.desc);                     // 第二行描述，delegate 画
    name->setData(Qt::UserRole + 2, expanded);                   // 决定是否画 ▾
    m_procTable->setItem(row, 0, name);

    setRightCell(m_procTable, row, 1, formatRate(s.downRate), ts::cDown());
    setRightCell(m_procTable, row, 2, formatRate(s.upRate), ts::cUp());
    setRightCell(m_procTable, row, 3, formatBytes(s.downBytes), ts::cTextSub());
    setRightCell(m_procTable, row, 4, formatBytes(s.upBytes), ts::cTextSub());
    // 连接数用橙色标一下（界面里就它一个是"个数"，标色后一眼能找到）
    setRightCell(m_procTable, row, 5, QString::number(s.connCount), ts::theme().warn);

    // 「操作」列：打开位置 + 强制结束进程（同名多进程会一起结束）
    auto* open = new QPushButton(QStringLiteral("打开位置"));
    open->setObjectName(QStringLiteral("openBtn"));
    open->setCursor(Qt::PointingHandCursor);
    const quint64 pid = s.pid;
    open->setEnabled(!ProcTraffic::instance().pathFor(pid).isEmpty());
    connect(open, &QPushButton::clicked, this, [pid]() {
        const QString p = ProcTraffic::instance().pathFor(pid);
        if (p.isEmpty())
            return;
        // /select, 只能给路径加引号：explorer 按原始命令行解析，
        // 若把 "/select,C:\..." 整体引起来（QProcess 会给含空格的参数整体加引号），
        // explorer 会把它当成一个路径而找不到 → 落到默认「文档」目录。
        // 因此自己拼命令行，交给 ShellExecuteW。
        const QString params =
            QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(p));
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe",
                        reinterpret_cast<const wchar_t*>(params.utf16()),
                        nullptr, SW_SHOWNORMAL);
    });

    const ProcStat snap = s;
    auto* kill = dangerButton(QStringLiteral("强制结束进程"));
    kill->setEnabled(!ProcTraffic::instance().pidsForName(s.name).isEmpty());
    connect(kill, &QPushButton::clicked, this, [this, snap]() { onKillProcess(snap); });

    auto* box = new QWidget;
    auto* lay = new QHBoxLayout(box);
    lay->setContentsMargins(4, 0, 4, 0);
    lay->setSpacing(4);
    lay->addWidget(open);
    lay->addWidget(kill);
    lay->addStretch(1);   // 右边留点余量，✕ 不贴死列边
    m_procTable->setCellWidget(row, 6, box);
}

void DetailDialog::fillPidRow(int row, const RowPlan& p)
{
    m_procTable->setRowHeight(row, kPidRowH);

    auto* item = new QTableWidgetItem;
    item->setData(Qt::UserRole + 7, quint64(p.pidStat.pid));
    item->setData(Qt::UserRole + 8, ProcTraffic::instance().categoryFor(p.pidStat.pid));
    m_procTable->setItem(row, 0, item);

    // 与该 PID 的速率 / 累计 / 连接数（列与进程行一致，表头不用变）
    setRightCell(m_procTable, row, 1, formatRate(p.pidStat.downRate), ts::cDown());
    setRightCell(m_procTable, row, 2, formatRate(p.pidStat.upRate), ts::cUp());
    setRightCell(m_procTable, row, 3, formatBytes(p.pidStat.downBytes), ts::cTextSub());
    setRightCell(m_procTable, row, 4, formatBytes(p.pidStat.upBytes), ts::cTextSub());
    setRightCell(m_procTable, row, 5, QString::number(p.pidStat.connCount), ts::theme().warn);

    const quint64 pid = p.pidStat.pid;
    const QString name = (p.procIdx >= 0 && p.procIdx < m_current.size())
                             ? m_current.at(p.procIdx).name
                             : QString();
    auto* kill = dangerButton(QStringLiteral("结束这个进程"));
    connect(kill, &QPushButton::clicked, this, [this, pid, name]() { onKillPid(pid, name); });
    m_procTable->setCellWidget(row, 6, centeredButton(kill));
}

void DetailDialog::fillNoteRow(int row, const QString& text)
{
    m_procTable->setRowHeight(row, kNoteRowH);
    auto* item = new QTableWidgetItem(text);
    item->setData(Qt::UserRole + 10, true);   // 交给 delegate 画成灰字
    m_procTable->setItem(row, 0, item);
    m_procTable->setSpan(row, 0, 1, 6);
}

void DetailDialog::fillSiteRow(int row, const SiteStat& s, bool expanded)
{
    // 展开标记与进程页同一套语义：▸ 未展开 / ▾ 已展开（这里直接写在文本里，
    // 不像进程页那样用 delegate —— 网站名只有一行，没必要多画一层）
    QString text = (expanded ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) + s.site;
    if (s.shared)
        text += QStringLiteral(" *");   // 含共用 CDN，归属可能不准（表头 ⓘ 有说明）

    auto* name = new QTableWidgetItem(text);
    name->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    name->setForeground(ts::cText());
    m_siteTable->setItem(row, 0, name);

    setRightCell(m_siteTable, row, 1, formatRate(s.downRate), ts::cDown());
    setRightCell(m_siteTable, row, 2, formatRate(s.upRate), ts::cUp());
    setRightCell(m_siteTable, row, 3, formatBytes(s.downBytes), ts::cTextSub());
    setRightCell(m_siteTable, row, 4, formatBytes(s.upBytes), ts::cTextSub());
}

void DetailDialog::fillSiteMemberRow(int row, const SiteMember& m)
{
    auto* name = new QTableWidgetItem(QStringLiteral("    └ ") + m.name);
    name->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    name->setForeground(ts::cTextSub());
    m_siteTable->setItem(row, 0, name);

    setRightCell(m_siteTable, row, 1, formatRate(m.downRate), ts::cDown());
    setRightCell(m_siteTable, row, 2, formatRate(m.upRate), ts::cUp());
    setRightCell(m_siteTable, row, 3, formatBytes(m.downBytes), ts::cTextSub());
    setRightCell(m_siteTable, row, 4, formatBytes(m.upBytes), ts::cTextSub());
}

void DetailDialog::onSiteRowClicked(QTableWidgetItem* item)
{
    if (!item)
        return;
    const int row = item->row();
    if (row < 0 || row >= m_sitePlan.size())
        return;
    const SiteRow& r = m_sitePlan.at(row);
    // 只有网站行可展开；域名明细行点它没意义
    if (r.kind != SiteRow::Site || r.siteIdx < 0 || r.siteIdx >= m_sites.size())
        return;

    const QString& site = m_sites.at(r.siteIdx).site;
    if (m_siteExpanded.contains(site))
        m_siteExpanded.remove(site);
    else
        m_siteExpanded.insert(site);
    refresh(false);
}

void DetailDialog::toggleRow(int row)
{
    if (row < 0 || row >= m_plan.size())
        return;
    const RowPlan& p = m_plan.at(row);
    if (p.procIdx < 0 || p.procIdx >= m_current.size())
        return;

    // 点提示行也算点进程行：直接收起，省得原地不动看着像没反应
    const QString& name = m_current.at(p.procIdx).name;
    if (m_expanded.contains(name))
        m_expanded.remove(name);
    else
        m_expanded.insert(name);
    refresh(false);
}

void DetailDialog::onProcRowClicked(QTableWidgetItem* item)
{
    if (!item)
        return;
    toggleRow(item->row());
}

void DetailDialog::onKillPid(quint64 pid, const QString& name)
{
    if (pid == 0)
        return;
    const QString label = name.isEmpty() ? QString::number(pid)
                                         : QStringLiteral("%1（PID %2）").arg(name).arg(pid);
    if (!ts::askConfirm(this,
                        QStringLiteral("TrafficScope - 结束进程"),
                        QStringLiteral("确定要强制结束这个进程吗？\n\n"
                                       "%1\n\n"
                                       "进程会被直接结束，"
                                       "未保存的数据会丢失，无法撤销。")
                            .arg(label),
                        QStringLiteral("强制结束")))
        return;

    if (!ProcTraffic::terminateProcess(pid)) {
        ts::showCardMessage(this, QStringLiteral("TrafficScope - 结束进程"),
                            QStringLiteral("结束失败：%1\n\n"
                                           "它可能是系统进程、"
                                           "其他账户的进程，"
                                           "或者已经退出。")
                                .arg(label));
    }
    refresh(false);
}

void DetailDialog::onKillProcess(const ProcStat& s)
{
    const QVector<quint64> pids = ProcTraffic::instance().pidsForName(s.name);
    if (pids.isEmpty())
        return;

    if (!ts::askConfirm(this,
                        QStringLiteral("TrafficScope - 结束进程"),
                        QStringLiteral("确定要强制结束以下进程吗？\n\n"
                                       "%1（%2 个进程）\n\n"
                                       "进程会被直接结束，"
                                       "未保存的数据会丢失，无法撤销。")
                            .arg(s.name)
                            .arg(pids.size()),
                        QStringLiteral("强制结束")))
        return;

    int failed = 0;
    for (quint64 pid : pids) {
        if (!ProcTraffic::terminateProcess(pid))
            ++failed;
    }
    if (failed > 0) {
        ts::showCardMessage(this, QStringLiteral("TrafficScope - 结束进程"),
                            QStringLiteral("共 %1 个进程，其中 %2 个结束失败：\n\n"
                                           "它们可能是系统进程、"
                                           "其他账户的进程，"
                                           "或者已经退出。")
                                .arg(pids.size())
                                .arg(failed));
    }
    refresh(false);
}

namespace {
// Qt6 弃用了 QMouseEvent::pos()/globalPos()，这里统一一下（Qt5/Qt6 通吃）
inline QPoint mouseLocalPos(const QMouseEvent* e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position().toPoint();
#else
    return e->pos();
#endif
}

inline QPoint mouseGlobalPos(const QMouseEvent* e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->globalPosition().toPoint();
#else
    return e->globalPos();
#endif
}

inline QPoint hoverGlobalPos(const QHoverEvent* e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->globalPosition().toPoint();
#else
    return e->globalPos();
#endif
}
}  // namespace

// 命中窗口四周边缘：四周 kShadowPad 的阴影留白区正好在卡片之外，
// 不会与自绘标题栏、表格抢事件。
// 说明：Qt 对 FramelessWindowHint 窗口会自行把 WM_NCHITTEST 判成 HTCLIENT，
// 所以调整大小必须由本类自己实现（不依赖 nativeEvent 的 HT*）。
int DetailDialog::edgeAt(const QPoint& pos) const
{
    const int d = kEdgeBand;   // 比卡片边缘再宽几 px，四角也更好抓
    int e = kEdgeNone;
    if (pos.x() < d)
        e |= kEdgeLeft;
    else if (pos.x() >= width() - d)
        e |= kEdgeRight;
    if (pos.y() < d)
        e |= kEdgeTop;
    else if (pos.y() >= height() - d)
        e |= kEdgeBottom;
    return e;
}

void DetailDialog::updateEdgeCursor(int edges)
{
    switch (edges) {
    case kEdgeLeft:
    case kEdgeRight:
        setCursor(Qt::SizeHorCursor);
        break;
    case kEdgeTop:
    case kEdgeBottom:
        setCursor(Qt::SizeVerCursor);
        break;
    case kEdgeLeft | kEdgeTop:
    case kEdgeRight | kEdgeBottom:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case kEdgeRight | kEdgeTop:
    case kEdgeLeft | kEdgeBottom:
        setCursor(Qt::SizeBDiagCursor);
        break;
    default:
        unsetCursor();
        break;
    }
}

void DetailDialog::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        const int edges = edgeAt(mouseLocalPos(e));
        if (edges != kEdgeNone) {
            m_resizeEdges = edges;
            m_pressGlobal = mouseGlobalPos(e);
            m_pressGeom = geometry();
            e->accept();
            return;
        }
    }
    QDialog::mousePressEvent(e);
}

void DetailDialog::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_resizeEdges) {
        updateEdgeCursor(edgeAt(mouseLocalPos(e)));
        QDialog::mouseMoveEvent(e);
        return;
    }

    // 拖拽途中左键已抬起（释放事件被别的窗口吃掉）→ 结束，避免窗口黏住光标
    if (!(e->buttons() & Qt::LeftButton)) {
        m_resizeEdges = kEdgeNone;
        return;
    }

    const QPoint d = mouseGlobalPos(e) - m_pressGlobal;
    QRect g = m_pressGeom;
    if (m_resizeEdges & kEdgeLeft)
        g.setLeft(g.left() + d.x());
    if (m_resizeEdges & kEdgeRight)
        g.setRight(g.right() + d.x());
    if (m_resizeEdges & kEdgeTop)
        g.setTop(g.top() + d.y());
    if (m_resizeEdges & kEdgeBottom)
        g.setBottom(g.bottom() + d.y());

    // 夹取最小尺寸：收的是被拖动的那一侧
    if (g.width() < minimumWidth()) {
        if (m_resizeEdges & kEdgeLeft)
            g.setLeft(g.right() - minimumWidth() + 1);
        else
            g.setRight(g.left() + minimumWidth() - 1);
    }
    if (g.height() < minimumHeight()) {
        if (m_resizeEdges & kEdgeTop)
            g.setTop(g.bottom() - minimumHeight() + 1);
        else
            g.setBottom(g.top() + minimumHeight() - 1);
    }
    setGeometry(g);
}

void DetailDialog::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_resizeEdges) {
        m_resizeEdges = kEdgeNone;
        updateEdgeCursor(edgeAt(mouseLocalPos(e)));
        // 记住调整后的大小，下次打开沿用（与浮框位置记忆共用程序根目录的 settings.ini）
        ts::setIni(QStringLiteral("Dialog/size"), size());
        return;
    }
    QDialog::mouseReleaseEvent(e);
}

bool DetailDialog::event(QEvent* ev)
{
    switch (ev->type()) {
    case QEvent::HoverMove:
        // 鼠标可能停在表格等子控件上方，用全局坐标换算回窗口坐标再判边缘
        if (!m_resizeEdges)
            updateEdgeCursor(edgeAt(mapFromGlobal(
                hoverGlobalPos(static_cast<QHoverEvent*>(ev)))));
        break;
    case QEvent::HoverLeave:
        if (!m_resizeEdges)
            unsetCursor();
        break;
    default:
        break;
    }
    return QDialog::event(ev);
}

void DetailDialog::leaveEvent(QEvent* e)
{
    if (!m_resizeEdges)
        unsetCursor();
    QDialog::leaveEvent(e);
}

void DetailDialog::restyle()
{
    // 搜索框样式与设置弹窗的过滤框共用
    setStyleSheet(ts::themed(QLatin1String(kDialogQss)) + ts::capQss()
                  + ts::lineEditQss());
    setPalette(ts::theme().palette());
    // 自绘的表格/表头不认 QSS，得自己重画
    ts::repaintAll(this);
}

bool SearchBox::active() const
{
    return edit && !edit->text().trimmed().isEmpty();
}

QString SearchBox::filter() const
{
    return active() ? edit->text().trimmed() : QString();
}

// 在页面工具行左侧摆上「搜索框 + 计数标签」；history=true 时关键字改动只重填历史表
void DetailDialog::addSearchBox(QWidget* parent, QHBoxLayout* lay, SearchBox& box,
                                const QString& placeholder, bool history)
{
    auto* edit = new QLineEdit(parent);
    edit->setPlaceholderText(placeholder);
    edit->setFixedSize(kSearchW, kSearchH);   // 四个页面同宽
    edit->setToolTip(QStringLiteral("空格分开的多个词要全部命中；Esc 清空"));
    edit->installEventFilter(this);   // Esc：先清关键字
    connect(edit, &QLineEdit::textChanged, this, [this, history]() {
        if (history)
            fillHistoryTable();   // 历史页只重算内存里的行，本来就不查库
        else
            refresh(false);       // 只按关键字重建表格，不喂顶部曲线
    });

    box.edit = edit;
    lay->addWidget(edit);
    if (!history) {
        auto* hint = new QLabel(parent);
        hint->setObjectName(QStringLiteral("searchHint"));
        hint->hide();
        box.hint = hint;
        lay->addWidget(hint);
    }
    lay->addSpacing(8);
}

// 计数标签「显示 N / M 项」，仅在有关键字时出现
void DetailDialog::updateSearchHint(const SearchBox& box, int shown, int total)
{
    if (!box.hint)
        return;
    if (!box.active() || total <= 0) {
        box.hint->hide();
        return;
    }
    box.hint->setText(QStringLiteral("显示 %1 / %2 项").arg(shown).arg(total));
    box.hint->show();
}

// 「没检测到浏览器扩展」= 本次运行没收到过扩展数据，或开关本身就关着。
void DetailDialog::updateSiteExtHint()
{
    if (!m_siteExtHint)
        return;
    const bool connected = ts::browserAttribution()
                           && ts::SiteBridge::instance().lastFeedMs() > 0;
    m_siteExtHint->setVisible(!connected);
}

// 搜索框按 Esc：有字先清空，空了才关窗
bool DetailDialog::eventFilter(QObject* watched, QEvent* e)
{
    if (e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape) {
            auto* edit = qobject_cast<QLineEdit*>(watched);
            if (edit && !edit->text().isEmpty()) {
                edit->clear();
                return true;   // 只清关键字，不关窗
            }
        }
    }
    return QDialog::eventFilter(watched, e);
}

// 无边框 + 透明顶层窗口：QSS 的背景不渲染，卡片与外阴影必须手绘
void DetailDialog::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    ts::drawCard(p, rect(), ts::kCardRadius, ts::theme().dialogCard, ts::cBorder());
}

void DetailDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    // 弹窗是复用的（关闭不销毁），再次显示时必须重拉一次，否则一直停在旧快照
    if (m_tabs && m_tabs->currentIndex() == kHistTab)
        refreshHistory();
    // 刷新周期可能在弹窗关闭期间被改过，重新显示时跟一次（读 ini，不能每拍读）
    m_timer->setInterval(ts::floatRefreshMs());
}

void DetailDialog::hideEvent(QHideEvent* e)
{
    if (m_tipCard)
        m_tipCard->dismiss();   // ⓘ 气泡是独立顶层窗口，不能留在屏幕上
    if (m_toastTimer)
        m_toastTimer->stop();   // 否则再显示时会被上一次的计时收掉
    QDialog::hideEvent(e);
}

void DetailDialog::resizeEvent(QResizeEvent* e)
{
    QDialog::resizeEvent(e);
    placeCopyToast();
}

void DetailDialog::setHistoryRange(int days)
{
    m_histRange = days;
    refreshHistory();
}

void DetailDialog::setHistoryDim(int dim)
{
    m_histDim = dim;
    m_histAppBtn->setChecked(dim == 0);
    m_histDomainBtn->setChecked(dim == 1);
    m_histSiteBtn->setChecked(dim == 2);
    refreshHistory();
}

void DetailDialog::onRangePicked(const QDate& d)
{
    if (m_pickingFrom) {
        m_customFrom = d;
        if (m_customTo < d)
            m_customTo = d;
    } else {
        m_customTo = d;
        if (m_customFrom > d)
            m_customFrom = d;
    }
    m_histRange = -1;   // 手动点日历即自定义区间
    refreshHistory();
}

void DetailDialog::refreshHistory()
{
    // 先把当前增量落盘再查，保证读到的是最新数据。
    // 周期采样仍是 60s，这里只是提前写入同一小时桶，不额外占用内存。
    StatsRecorder::instance().flushNow();

    StatsDb& db = StatsDb::instance();
    if (!db.isOpen()) {
        m_histSummary->setText(QStringLiteral("历史统计不可用（数据库未打开）"));
        m_histDown->setText(QStringLiteral("0B"));
        m_histUp->setText(QStringLiteral("0B"));
        m_histTable->setRowCount(0);
        m_histRows.clear();       // 缓存一并清掉
        m_histSeries.clear();
        m_histRangeText.clear();
        return;
    }

    const QDate today = QDate::currentDate();
    QDate from;
    QDate to;
    if (m_histRange == -2) {                       // 全部
        const qint64 first = db.earliestTs();
        from = first > 0 ? QDateTime::fromSecsSinceEpoch(first).date() : today;
        to = today;
    } else if (m_histRange < 0) {                  // 自定义
        from = m_customFrom;
        to = m_customTo;
        if (from > to)
            from = to;
    } else {                                       // 今日 / 近N天
        to = today;
        from = today.addDays(m_histRange == 0 ? 0 : -(m_histRange - 1));
    }

    const qint64 fromTs = dayStart(from);
    const qint64 toTs = dayStart(to.addDays(1));
    const bool byApp = (m_histDim == 0);
    // 网站维度查 site 表（落库时就是"当时归属到的网站"，数字不会随归属变化）；
    QVector<StatsDb::Row> rows =
        byApp ? db.queryApps(fromTs, toTs, kHistRows)
              : m_histDim == 2 ? db.querySites(fromTs, toTs, kHistRows)
                               : db.queryDomains(fromTs, toTs, kHistRows);

    // 设置项：不再展示未知流量 —— 只过滤显示，库里的记录保留（不是删除）。
    // 用 isUnattributedName() 而不是直接比名字：改名前的历史记录叫"未归因流量"，
    // 直接比会让那些旧记录又冒出来。
    if (byApp && !TrafficStore::instance().countsUnattributed()) {
        QVector<StatsDb::Row> kept;
        for (const StatsDb::Row& r : rows) {
            if (!ProcTraffic::isUnattributedName(r.name))
                kept.append(r);
        }
        rows = kept;
    }

    sortRows(rows, m_histSort, histLess);   // 按表头点过的列排序（数值列比数值）

    // 缓存本次查询结果，供改关键字时重填表格
    m_histRows = rows;

    // 下载 / 上传总量与「占比」列的分母都在 fillHistoryTable() 里按**当前关键字**
    // 算（改关键字时跟着变），这里只负责区间两端的日期文本。
    m_fromBtn->setText(from.toString(QStringLiteral("yyyy-MM-dd")));
    m_toBtn->setText(to.toString(QStringLiteral("yyyy-MM-dd")));

    const int days = from.daysTo(to) + 1;
    m_histRangeText =
        QStringLiteral("区间 %1 ~ %2（%3 天，%4 条记录）")
            .arg(from.toString(QStringLiteral("yyyy-MM-dd")),
                 to.toString(QStringLiteral("yyyy-MM-dd")))
            .arg(days)
            .arg(rows.size());

    // 趋势图分桶：短区间按小时，长区间按天。
    // 不用更细的粒度是刻意的 —— 老区间的历史数据可能只剩按天存的桶，
    // 再按小时切只会把整天的数据全堆到 0 点那一格。
    // 必须带 UTC 偏移：库里的桶是**本地时间**对齐的，seriesByName() 里按
    // ((ts + off) / bucket) * bucket - off 归桶，偏移传 0 会按 UTC 分桶（UTC+8 下差 8 小时）。
    m_histByDay = days > 2;
    const qint64 off = QDateTime::currentDateTime().offsetFromUtc();
    // 网站维度趋势直接查 site 表（与表格同一来源）
    m_histSeries = db.seriesByName(fromTs, toTs, m_histDim,
                                   m_histByDay ? 86400 : 3600, off);

    fillHistoryTable();
}

// 按缓存的 m_histRows + 当前关键字重填历史表（不查库）
void DetailDialog::fillHistoryTable()
{
    const QString filter = m_histSearch.filter();
    QVector<int> shown;
    shown.reserve(m_histRows.size());
    for (int i = 0; i < m_histRows.size(); ++i) {
        if (matchFilter(filter, m_histRows.at(i).name))
            shown.append(i);
    }

    // 总量与「占比」的分母都按**筛出来的行**算：不打关键字时等于整区间，
    // 打了关键字就只算命中的那些名称 —— 所以筛选时占比列合计仍是 100%。
    quint64 sumUp = 0;
    quint64 sumDown = 0;
    for (int i = 0; i < shown.size(); ++i) {
        sumUp += m_histRows.at(shown.at(i)).up;
        sumDown += m_histRows.at(shown.at(i)).down;
    }
    const quint64 shownTotal = sumUp + sumDown;
    m_histDown->setText(formatBytes(sumDown));
    m_histUp->setText(formatBytes(sumUp));

    m_histTable->setRowCount(shown.size());
    for (int row = 0; row < shown.size(); ++row) {
        const StatsDb::Row& r = m_histRows.at(shown.at(row));
        const quint64 sum = r.up + r.down;

        auto* name = new QTableWidgetItem(r.name);
        name->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        name->setForeground(ts::cText());
        m_histTable->setItem(row, 0, name);

        setRightCell(m_histTable, row, 1, formatBytes(r.down), ts::cDown());
        setRightCell(m_histTable, row, 2, formatBytes(r.up), ts::cUp());
        setRightCell(m_histTable, row, 3, formatBytes(sum), ts::cText());
        setRightCell(m_histTable, row, 4,
                     shownTotal > 0
                         ? QString::number(double(sum) * 100.0 / double(shownTotal), 'f', 1)
                               + QLatin1String("%")
                         : QStringLiteral("-"),
                     ts::cTextSub());
    }
    m_histSummary->setText(m_histSearch.active()
                               ? m_histRangeText + QStringLiteral("，显示 %1 项").arg(shown.size())
                               : m_histRangeText);
    updateHistChart();
}

// 趋势柱状图跟着关键字走：只汇总表格里显示的名称，明细都在内存里（不查库）
void DetailDialog::updateHistChart()
{
    if (!m_histChart)
        return;

    const QString filter = m_histSearch.filter();
    QSet<QString> keep;
    if (!filter.isEmpty()) {
        keep.reserve(m_histRows.size());
        for (const StatsDb::Row& r : m_histRows) {
            if (matchFilter(filter, r.name))
                keep.insert(r.name);
        }
    }

    QVector<StatsDb::Point> pts;
    QHash<qint64, int> at;
    for (const StatsDb::NamePoint& p : m_histSeries) {
        if (!filter.isEmpty() && !keep.contains(p.name))
            continue;
        int idx = at.value(p.ts, -1);
        if (idx < 0) {
            idx = pts.size();
            at.insert(p.ts, idx);
            StatsDb::Point np;
            np.ts = p.ts;
            pts.append(np);
        }
        pts[idx].up += p.up;
        pts[idx].down += p.down;
    }
    std::sort(pts.begin(), pts.end(),
              [](const StatsDb::Point& a, const StatsDb::Point& b) { return a.ts < b.ts; });
    m_histChart->setData(pts, m_histByDay);
}

void DetailDialog::connectSort(QTableWidget* t, SortState& st)
{
    connect(t->horizontalHeader(), &QHeaderView::sectionClicked,
            this, [this, t, &st](int col) { onSortClicked(t, st, col); });
}

void DetailDialog::onSortClicked(QTableWidget* t, SortState& st, int col)
{
    if (col < 0 || col >= st.cols)   // "操作"这类按钮列不参与排序
        return;
    if (st.col == col) {
        st.order = (st.order == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
    } else {
        st.col = col;
        // 名称列默认升序（A→Z），数值列默认降序（先看"谁最占流量"）
        st.order = (col == 0) ? Qt::AscendingOrder : Qt::DescendingOrder;
    }
    t->horizontalHeader()->setSortIndicator(col, st.order);
    t->horizontalHeader()->setSortIndicatorShown(true);
    // 立刻重刷，不用等下一次 tick（历史表 10s、实时表 1s）
    if (t == m_histTable)
        refreshHistory();
    else
        refresh(false);   // 点表头排序只重建表格，别给顶部曲线补采样点
}

// ---- 右键复制 ----
void DetailDialog::attachCopyMenu(QTableWidget* t)
{
    t->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(t, &QTableWidget::customContextMenuRequested,
            this, [this, t](const QPoint& pos) { showCellMenu(t, pos); });
}

void DetailDialog::showCellMenu(QTableWidget* t, const QPoint& pos)
{
    if (!t)
        return;
    const QTableWidgetItem* hit = t->itemAt(pos);
    if (!hit)
        return;   // 「操作」列是 cellWidget（按钮），底下没有 item
    const int row = hit->row();
    const int col = hit->column();
    t->setCurrentCell(row, col);   // 高亮跟着右键走，不用等左键

    const QString cell = cellText(t, row, col);
    const QString line = rowText(t, row);
    if (cell.isEmpty() && line.isEmpty())
        return;

    QMenu menu(this);
    QAction* copyCell = menu.addAction(QStringLiteral("复制单元格"));
    copyCell->setEnabled(!cell.isEmpty());
    // 提示行的内容全在第 0 列，"整行"跟"单元格"没区别，这时不显示第二项
    QAction* copyRow = line.contains(QLatin1Char('\t'))
                           ? menu.addAction(QStringLiteral("复制整行")) : nullptr;
    ts::polishMenu(menu);
    connect(copyCell, &QAction::triggered, this, [this, cell]() { copyToClipboard(cell); });
    if (copyRow)
        connect(copyRow, &QAction::triggered, this, [this, line]() { copyToClipboard(line); });
    menu.exec(t->viewport()->mapToGlobal(pos));
}

// 复制的是显示文本：首列自绘（PID 行 item 为空），网站页还带 ▸/▾ 与缩进装饰
QString DetailDialog::cellText(QTableWidget* t, int row, int col) const
{
    if (!t || row < 0 || col < 0)
        return QString();
    const QTableWidgetItem* item = t->item(row, col);
    QString text = item ? item->text() : QString();
    if (col != 0)
        return text;

    if (t == m_procTable) {
        if (row < m_plan.size() && m_plan.at(row).kind == RowPlan::Pid)
            return QStringLiteral("进程ID: %1").arg(m_plan.at(row).pidStat.pid);
        return text;
    }
    if (t == m_siteTable) {
        if (text.startsWith(QStringLiteral("▸ ")) || text.startsWith(QStringLiteral("▾ ")))
            return text.mid(2);
        text = text.trimmed();
        if (text.startsWith(QStringLiteral("└ ")))
            text = text.mid(2);
    }
    return text;
}

// 整行：各列显示文本用 Tab 拼起来；「操作」列没有 item，尾部空列要丢掉
QString DetailDialog::rowText(QTableWidget* t, int row) const
{
    if (!t || row < 0)
        return QString();
    QStringList cells;
    for (int col = 0; col < t->columnCount(); ++col)
        cells << cellText(t, row, col);
    while (!cells.isEmpty() && cells.last().isEmpty())
        cells.removeLast();
    return cells.join(QLatin1Char('\t'));
}

void DetailDialog::copyToClipboard(const QString& text)
{
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    showCopyToast();
}

void DetailDialog::showCopyToast()
{
    if (!m_toast || !m_toastTimer)
        return;
    m_toast->setText(QStringLiteral("已复制"));
    m_toast->show();
    placeCopyToast();
    m_toast->raise();        // 浮在表格之上（子控件是按创建顺序堆叠的）
    m_toastTimer->start();
}

// 卡片外圈的 kShadowPad 阴影留白四边等宽，取窗口中心即卡片中心
void DetailDialog::placeCopyToast()
{
    if (!m_toast || !m_toast->isVisible())
        return;
    m_toast->adjustSize();
    m_toast->move((width() - m_toast->width()) / 2,
                  (height() - m_toast->height()) / 2);
}

// 导出当前表为 CSV（表头 + 全部行）。
//
// 两个必须照顾到的点，踩了都是"文件生成了但没法用"：
//  ① **UTF-8 BOM**：不写 BOM，Excel 会按系统本地编码（GBK）解析 → 中文全是乱码；
//  ② RFC 4180 转义：字段里含逗号 / 双引号 / 换行时用双引号包起来，内部引号写成两个。
void DetailDialog::exportTable(QTableWidget* t, const QString& defaultName)
{
    if (!t)
        return;

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 CSV"),
        QStringLiteral("TrafficScope-%1-%2.csv").arg(defaultName, stamp),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ts::showCardMessage(this, QStringLiteral("导出失败"),
                            QStringLiteral("无法写入文件：\n%1").arg(path));
        return;
    }

    QTextStream out(&f);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    out.setEncoding(QStringConverter::Utf8);   // Qt6 里 setCodec 已移除
#else
    out.setCodec("UTF-8");
#endif
    out << QChar(0xFEFF);   // BOM

    auto esc = [](const QString& s) -> QString {
        const bool need = s.contains(QLatin1Char(','))
                       || s.contains(QLatin1Char('"'))
                       || s.contains(QLatin1Char('\n'))
                       || s.contains(QLatin1Char('\r'));
        if (!need)
            return s;
        QString r = s;
        r.replace(QLatin1Char('"'), QLatin1String("\"\""));
        return QLatin1Char('"') + r + QLatin1Char('"');
    };

    const int cols = t->columnCount();
    for (int c = 0; c < cols; ++c) {
        if (c)
            out << ',';
        const QTableWidgetItem* h = t->horizontalHeaderItem(c);
        out << esc(h ? h->text() : QString());
    }
    out << "\r\n";

    for (int r = 0; r < t->rowCount(); ++r) {
        for (int c = 0; c < cols; ++c) {
            if (c)
                out << ',';
            const QTableWidgetItem* it = t->item(r, c);
            out << esc(it ? it->text() : QString());
        }
        out << "\r\n";
    }

    out.flush();
    const bool ok = (f.error() == QFile::NoError);
    f.close();
    if (!ok)
        ts::showCardMessage(this, QStringLiteral("导出失败"),
                            QStringLiteral("写入文件时出错：\n%1").arg(path));
}
