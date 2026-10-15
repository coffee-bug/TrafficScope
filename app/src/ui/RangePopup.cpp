#include "RangePopup.h"

#include "UiTheme.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <functional>

namespace {

constexpr int kCellW = 36;   // 单个日期格宽
constexpr int kCellH = 30;   // 单个日期格高
constexpr int kHeadH = 24;   // 星期表头高

// 周日排第一列
const char* const kWeekdays[] = {"日", "一", "二", "三",
                                 "四", "五", "六"};

// 色值全是 %TOKEN%，由 ts::themed() 按当前主题替换（见 UiTheme.h）
const char* kPopupQss =
    "QWidget { background: %POPUP_BG%; font-family: \"%FONT%\"; }"
    "QLabel#monthTitle { color: %TEXT%; font-size: 12px; font-weight: 600; }"
    // 快捷按钮
    "QPushButton { background: transparent; color: %TEXT_SUB%;"
    " border: 1px solid %FIELD_BORDER%; border-radius: 6px;"
    " padding: 4px 12px; font-size: 12px; }"
    "QPushButton:hover { color: %TEXT%; border-color: %FIELD_BORDER_HOVER%; }"
    "QPushButton:checked { background: %ACCENT_SOFT%; color: %TEXT%;"
    " border-color: %ACCENT%; }"
    // 翻月按钮（后写以覆盖上面的通用样式）
    "QPushButton#navBtn { background: transparent; color: %TEXT2%;"
    " border: none; border-radius: 4px; padding: 0; font-size: 15px; }"
    "QPushButton#navBtn:hover { background: %POPUP_SEL%; color: %TEXT%; }"
    // 橙色确定
    "QPushButton#confirmBtn { background: %WARN%; color: %ON_ACCENT%;"
    " border: none; font-weight: 600; padding: 4px 14px; font-size: 12px; }"
    "QPushButton#confirmBtn:hover { background: %WARN_HOVER%; }";

QString monthTitle(int year, int month)
{
    return QStringLiteral("%1年%2月").arg(year).arg(month);
}

}  // namespace

// ---------------------------------------------------------------------------
// 单月网格：整块自绘（星期表头 + 6 行日期），拾取结果通过回调交给 RangePopup。
// 故意不放进匿名 namespace —— 头文件里有前向声明，类型必须同一个。
// ---------------------------------------------------------------------------
class MonthView : public QWidget {
public:
    explicit MonthView(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setFixedSize(kCellW * 7, kHeadH + kCellH * 6);
    }

    void setMonth(int year, int month)
    {
        m_year = year;
        m_month = month;
        update();
    }
    void setSelected(const QDate& d) { m_sel = d; update(); }
    void setToday(const QDate& d) { m_today = d; update(); }

    std::function<void(const QDate&)> onPicked;

protected:
    void paintEvent(QPaintEvent*) override
    {
        const ts::Theme& t = ts::theme();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), t.popupBg);   // 自己铺底，不依赖 QSS（透明顶层窗口的坑）

        QFont f(ts::fontFamily());
        f.setPixelSize(11);
        p.setFont(f);
        p.setPen(t.textFaint);
        for (int i = 0; i < 7; ++i)
            p.drawText(QRect(i * kCellW, 2, kCellW, kHeadH - 4),
                       Qt::AlignCenter, QString::fromUtf8(kWeekdays[i]));

        if (m_year <= 0 || m_month < 1 || m_month > 12)
            return;

        const QDate first(m_year, m_month, 1);
        const int lead = first.dayOfWeek() % 7;   // Qt: 周一=1..周日=7 -> 周日起 0
        const int days = first.daysInMonth();

        for (int d = 1; d <= days; ++d) {
            const int idx = lead + d - 1;
            const int row = idx / 7;
            const int col = idx % 7;
            if (row > 5)
                break;
            const QRect cell(col * kCellW, kHeadH + row * kCellH, kCellW, kCellH);
            const QDate cur(m_year, m_month, d);
            const bool future = m_today.isValid() && cur > m_today;
            const bool picked = m_sel.isValid() && m_sel == cur;

            if (picked) {
                p.setPen(Qt::NoPen);
                p.setBrush(t.accent);
                p.drawRoundedRect(QRectF(cell).adjusted(2, 2, -2, -2), 5, 5);
            } else if (m_hover.isValid() && m_hover == cur && !future) {
                p.setPen(Qt::NoPen);
                p.setBrush(t.rowHover);
                p.drawRoundedRect(QRectF(cell).adjusted(2, 2, -2, -2), 5, 5);
            }

            QColor tc = t.text2;
            if (picked)
                tc = t.onAccent;
            else if (future)
                tc = t.dateOff;
            else if (cur == m_today)
                tc = t.accent;

            f.setPixelSize(12);
            f.setWeight(cur == m_today ? QFont::DemiBold : QFont::Normal);
            p.setFont(f);
            p.setPen(tc);
            p.drawText(cell, Qt::AlignCenter, QString::number(d));
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const QDate d = dateAt(e->pos());
        if (d.isValid() && !(m_today.isValid() && d > m_today) && onPicked)
            onPicked(d);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QDate d = dateAt(e->pos());
        if (d != m_hover) {
            m_hover = d;
            update();
        }
    }

    void leaveEvent(QEvent*) override
    {
        if (m_hover.isValid()) {
            m_hover = QDate();
            update();
        }
    }

private:
    QDate dateAt(const QPoint& pos) const
    {
        if (pos.y() < kHeadH)
            return QDate();
        const int col = pos.x() / kCellW;
        const int row = (pos.y() - kHeadH) / kCellH;
        if (col < 0 || col > 6 || row < 0 || row > 5)
            return QDate();
        if (m_year <= 0 || m_month < 1 || m_month > 12)
            return QDate();
        const QDate first(m_year, m_month, 1);
        const int idx = row * 7 + col - (first.dayOfWeek() % 7) + 1;
        if (idx < 1 || idx > first.daysInMonth())
            return QDate();
        return QDate(m_year, m_month, idx);
    }

    int m_year = 0;
    int m_month = 0;
    QDate m_sel;
    QDate m_today;
    QDate m_hover;
};

// ---------------------------------------------------------------------------

RangePopup::RangePopup(QWidget* parent)
    : QWidget(parent, Qt::Popup)
{
    restyle();
    setAttribute(Qt::WA_StyledBackground);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 10);
    lay->setSpacing(8);

    // ---- 顶部翻月：‹ 左，› 右 ----
    auto* nav = new QWidget;
    auto* nl = new QHBoxLayout(nav);
    nl->setContentsMargins(0, 0, 0, 0);
    nl->setSpacing(6);
    auto mkNav = [this](const QString& text, int delta) -> QPushButton* {
        auto* b = new QPushButton(text);
        b->setObjectName(QStringLiteral("navBtn"));
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(24, 22);
        connect(b, &QPushButton::clicked, this, [this, delta]() { shiftMonth(delta); });
        return b;
    };
    nl->addWidget(mkNav(QStringLiteral("‹"), -1));
    nl->addStretch(1);
    nl->addWidget(mkNav(QStringLiteral("›"), 1));
    lay->addWidget(nav);

    // ---- 双月：每列 = 标题 + 网格 ----
    auto* grids = new QWidget;
    auto* gl = new QHBoxLayout(grids);
    gl->setContentsMargins(0, 0, 0, 0);
    gl->setSpacing(16);

    m_left = new MonthView;
    m_right = new MonthView;
    m_titleL = new QLabel;
    m_titleR = new QLabel;
    for (QLabel* t : {m_titleL, m_titleR}) {
        t->setObjectName(QStringLiteral("monthTitle"));
        t->setAlignment(Qt::AlignCenter);
        t->setFixedHeight(18);
    }
    auto* colL = new QWidget;
    auto* colR = new QWidget;
    auto* clL = new QVBoxLayout(colL);
    auto* clR = new QVBoxLayout(colR);
    clL->setContentsMargins(0, 0, 0, 0);
    clR->setContentsMargins(0, 0, 0, 0);
    clL->setSpacing(4);
    clR->setSpacing(4);
    clL->addWidget(m_titleL);
    clL->addWidget(m_left);
    clR->addWidget(m_titleR);
    clR->addWidget(m_right);
    gl->addWidget(colL);
    gl->addWidget(colR);
    lay->addWidget(grids);

    m_left->onPicked = [this](const QDate& d) { emit datePicked(d); close(); };
    m_right->onPicked = [this](const QDate& d) { emit datePicked(d); close(); };

    // ---- 底部快捷 + 确定 ----
    auto* row = new QWidget;
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(6);
    const struct {
        const char* text;
        int days;
    } quicks[] = {
        {"今天", 0},           // 今天
        {"最近3天", 3},    // 最近三天
        {"最近7天", 7},    // 最近七天
        {"最近30天", 30},  // 最近三十天
        {"全部", -2},          // 全部
    };
    for (const auto& q : quicks) {
        auto* b = new QPushButton(QString::fromUtf8(q.text));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        const int days = q.days;
        connect(b, &QPushButton::clicked, this, [this, days]() {
            emit quickRange(days);
            close();
        });
        rl->addWidget(b);
    }
    rl->addStretch(1);
    auto* confirm = new QPushButton(QStringLiteral("确定"));
    confirm->setObjectName(QStringLiteral("confirmBtn"));
    confirm->setCursor(Qt::PointingHandCursor);
    connect(confirm, &QPushButton::clicked, this, &RangePopup::close);
    rl->addWidget(confirm);
    lay->addWidget(row);

    connect(&ts::themeNotifier(), &ts::ThemeNotifier::changed,
            this, &RangePopup::restyle);
}

void RangePopup::restyle()
{
    setStyleSheet(ts::themed(QLatin1String(kPopupQss)));
    // 单月网格是整块自绘的（取主题色），必须重画
    ts::repaintAll(this);
}

void RangePopup::shiftMonth(int delta)
{
    const QDate d = QDate(m_year, m_month, 1).addMonths(delta);
    m_year = d.year();
    m_month = d.month();
    syncMonths();
}

void RangePopup::syncMonths()
{
    m_left->setMonth(m_year, m_month);
    const QDate r = QDate(m_year, m_month, 1).addMonths(1);
    m_right->setMonth(r.year(), r.month());
    m_titleL->setText(monthTitle(m_year, m_month));
    m_titleR->setText(monthTitle(r.year(), r.month()));
    m_left->setSelected(m_selected);
    m_right->setSelected(m_selected);
}

void RangePopup::popupFor(QWidget* anchor, const QDate& selected)
{
    // 每次打开都清掉上一轮的快捷按钮高亮，避免出现多个同时选中
    for (QPushButton* b : findChildren<QPushButton*>()) {
        if (b->objectName() != QLatin1String("confirmBtn"))
            b->setChecked(false);
    }

    const QDate today = QDate::currentDate();
    m_selected = selected.isValid() ? selected : today;
    // 左月 = 选中月的上一个月，保证"本月"永远在右侧可见
    const QDate left = QDate(m_selected.year(), m_selected.month(), 1).addMonths(-1);
    m_year = left.year();
    m_month = left.month();

    m_left->setToday(today);
    m_right->setToday(today);
    syncMonths();

    adjustSize();

    QPoint gp = anchor->mapToGlobal(QPoint(0, anchor->height() + 4));
    if (QScreen* s = QGuiApplication::screenAt(
            anchor->mapToGlobal(anchor->rect().center()))) {
        const QRect av = s->availableGeometry();
        if (gp.x() + width() > av.right())
            gp.setX(av.right() - width() - 8);
        if (gp.y() + height() > av.bottom())
            gp.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - height() - 4);
    }
    move(gp);
    show();
}
