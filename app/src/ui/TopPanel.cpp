#include "TopPanel.h"

#include "ProcIcon.h"
#include "UiTheme.h"
#include "core/ProcTraffic.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {
const int kRowsPerSection = 4;
// 卡片宽（widget = 卡片 + 2 * 阴影留白）。宽度由下面几项加起来定死：
// 左右留白 2*14 + 分区内边距 2*12 + 图标 16 + 间距 3*8 + 名称列 + 速率列 78 + 名称与速率之间约 12px 空隙。
// 名称列 194 时中间会空掉一大块（短进程名尤其明显），收到 140：常见名
// （WorkBuddy.exe / NVIDIA Overlay.exe 等）仍能完整显示，超长的才省略。
const int kCardWidth = 322;
const int kProcNameWidth = 140;
const int kRateWidth = 78;      // 放得下 "↓ 9.99MB/s"

const int kRowHeight = 22;      // 名称行 16 + 间距 3 + 占比条 3
// 分区高 = 内边距(10+10)20 + 标题 16 + 行间距 5×4 + 显式间距 2 + 4 行(4×22)88 = 146
const int kSectionHeight = 146;
const int kPageHeight = 302;    // 上传 Top 146 + 间距 10 + 下载 Top 146
const int kWrapX = 14;
const int kWrapTop = 14;
const int kWrapBottom = 12;
const int kSummaryH = 16;       // 「今日 ↓x ↑y」那一行
// 内容高 = 20(标题) + 10 + 16(摘要) + 10 + 302(榜单) + 10 + 26(查看详情按钮) = 394
const int kCardHeight = kWrapTop + 394 + kWrapBottom;

// 占比条：底槽与条色都取主题（%FIELD_BG_SOFT% 由 ts::themed() 替换）
QString accentBarQss(const QColor& c)
{
    return ts::themed(QStringLiteral(
                          "QProgressBar { background: %FIELD_BG_SOFT%; border: none;"
                          " border-radius: 2px; }"
                          "QProgressBar::chunk { background: %CHUNK%;"
                          " border-radius: 2px; }")
                          .replace(QStringLiteral("%CHUNK%"),
                                   c.name(QColor::HexArgb)));
}

// 面板 QSS（色值全是 %TOKEN%，见 UiTheme.h）
const char* kPanelQss =
    "#topPanel { font-family: \"%FONT%\"; }"
    "#panelTitle { color: %TEXT%; font-size: 13px; font-weight: 600; }"
    "#panelTotal { color: %DOWN%; font-size: 12px; font-weight: 600; }"
    "#sectionBox { background: %FIELD_BG%; border-radius: 10px; }"
    "#sectionHead { color: %TEXT_SUB%; font-size: 11px; font-weight: 600; }"
    "#rowName { color: %TEXT2%; font-size: 12px; }"
    "#rowRateUp { color: %UP%; font-size: 12px; font-weight: 600; }"
    "#rowRateDown { color: %DOWN%; font-size: 12px; font-weight: 600; }"
    "#panelSummary { color: %TEXT_SUB%; font-size: 11px; }"
    "#panelHint { color: %TEXT_FAINT%; font-size: 11px; }"
    "QPushButton#detailBtn { background: %FIELD_BG%;"
    " border: 1px solid %FIELD_BORDER%; border-radius: 8px;"
    " color: %TEXT2%; font-size: 12px; padding: 3px 0; }"
    "QPushButton#detailBtn:hover { background: %FIELD_BG_HOVER%;"
    " color: %TEXT%; }";
}  // namespace

TopPanel::TopPanel(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
                          | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setObjectName(QStringLiteral("topPanel"));
    setFixedSize(kCardWidth + ts::kShadowPad * 2,
                 kCardHeight + ts::kShadowPad * 2);
    // 样式（kPanelQss）统一在构造末尾的 restyle() 里套上：主题切换时复用同一入口

    const int mx = ts::kShadowPad + kWrapX;
    const int myTop = ts::kShadowPad + kWrapTop;
    const int myBot = ts::kShadowPad + kWrapBottom;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(mx, myTop, mx, myBot);
    root->setSpacing(10);

    // ---- 标题栏 ----
    auto* head = new QWidget;
    head->setFixedHeight(20);
    auto* headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(0, 0, 0, 0);
    headLay->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("TrafficScope"));
    title->setObjectName(QStringLiteral("panelTitle"));
    title->setFixedHeight(20);
    m_totalLabel = new QLabel(QStringLiteral("↓ 0B/s"));
    m_totalLabel->setObjectName(QStringLiteral("panelTotal"));
    m_totalLabel->setFixedHeight(20);
    headLay->addWidget(title);
    headLay->addStretch();
    headLay->addWidget(m_totalLabel);
    root->addWidget(head);

    // ---- 摘要行：今日用量 ----
    m_summaryLabel = new QLabel;
    m_summaryLabel->setObjectName(QStringLiteral("panelSummary"));
    m_summaryLabel->setFixedHeight(kSummaryH);
    root->addWidget(m_summaryLabel);

    // ---- 应用实时榜（上传 Top / 下载 Top）----
    m_procUpSection = makeSection(QStringLiteral("上传 Top"), ts::cUp(),
                                  QStringLiteral("rowRateUp"), m_procUpRows);
    m_procDownSection = makeSection(QStringLiteral("下载 Top"), ts::cDown(),
                                    QStringLiteral("rowRateDown"), m_procDownRows);
    m_procHint = new QLabel(QStringLiteral("正在监听网络流量…"));
    m_procHint->setObjectName(QStringLiteral("panelHint"));
    m_procHint->setAlignment(Qt::AlignCenter);
    m_procHint->setFixedHeight(120);
    m_procUpSection->hide();
    m_procDownSection->hide();

    auto* page = new QWidget;
    page->setFixedHeight(kPageHeight);
    auto* pageLay = new QVBoxLayout(page);
    pageLay->setContentsMargins(0, 0, 0, 0);
    pageLay->setSpacing(10);
    pageLay->addWidget(m_procUpSection);
    pageLay->addWidget(m_procDownSection);
    pageLay->addWidget(m_procHint);
    pageLay->addStretch(1);
    root->addWidget(page);

    // ---- 查看详情 ----
    auto* detailBtn = new QPushButton(QStringLiteral("查看详情"));
    detailBtn->setObjectName(QStringLiteral("detailBtn"));
    detailBtn->setCursor(Qt::PointingHandCursor);
    detailBtn->setFixedHeight(26);
    connect(detailBtn, &QPushButton::clicked, this, &TopPanel::detailRequested);
    root->addWidget(detailBtn);

    restyle();
    connect(&ts::themeNotifier(), &ts::ThemeNotifier::changed,
            this, &TopPanel::restyle);
}

void TopPanel::restyle()
{
    setStyleSheet(ts::themed(QLatin1String(kPanelQss)));
    // 每行占比条的配色也跟着主题走
    const QColor up = ts::cUp();
    const QColor down = ts::cDown();
    for (const RowUi& r : m_procUpRows)
        r.bar->setStyleSheet(accentBarQss(QColor(up.red(), up.green(), up.blue(), 210)));
    for (const RowUi& r : m_procDownRows)
        r.bar->setStyleSheet(accentBarQss(QColor(down.red(), down.green(), down.blue(), 210)));
    ts::repaintAll(this);
}

void TopPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    ts::drawCard(p, rect(), ts::kCardRadius, ts::cCard(), ts::cBorder());
}

QWidget* TopPanel::makeSection(const QString& title, const QColor& accent,
                               const QString& rateObj, QVector<RowUi>& rows)
{
    auto* box = new QWidget;
    box->setObjectName(QStringLiteral("sectionBox"));
    box->setFixedHeight(kSectionHeight);
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(4);

    auto* headWidget = new QWidget;
    headWidget->setFixedHeight(16);
    auto* headLay = new QHBoxLayout(headWidget);
    headLay->setContentsMargins(0, 0, 0, 0);
    headLay->setSpacing(6);
    auto* dot = new QLabel(QStringLiteral("●"));
    dot->setStyleSheet(QStringLiteral("color: %1; font-size: 8px;")
                           .arg(accent.name(QColor::HexArgb)));
    dot->setFixedHeight(16);
    auto* head = new QLabel(title);
    head->setObjectName(QStringLiteral("sectionHead"));
    head->setFixedHeight(16);
    headLay->addWidget(dot);
    headLay->addWidget(head);
    headLay->addStretch();
    lay->addWidget(headWidget);
    lay->addSpacing(2);

    const QString barQss = accentBarQss(
        QColor(accent.red(), accent.green(), accent.blue(), 210));

    for (int i = 0; i < kRowsPerSection; ++i) {
        RowUi r;
        r.box = new QWidget;
        r.box->setFixedHeight(kRowHeight);
        auto* rowLay = new QVBoxLayout(r.box);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(3);

        auto* line = new QHBoxLayout;
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        r.icon = new QLabel;
        r.icon->setFixedSize(16, 16);
        r.icon->setAlignment(Qt::AlignCenter);
        r.name = new QLabel;
        r.name->setObjectName(QStringLiteral("rowName"));
        r.name->setFixedSize(kProcNameWidth, 16);
        r.rate = new QLabel;
        r.rate->setObjectName(rateObj);
        r.rate->setFixedSize(kRateWidth, 16);
        r.rate->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        line->addWidget(r.icon);
        line->addWidget(r.name);
        line->addStretch(1);
        line->addWidget(r.rate);
        rowLay->addLayout(line);

        r.bar = new QProgressBar;
        r.bar->setFixedHeight(3);
        r.bar->setTextVisible(false);
        r.bar->setRange(0, 100);
        r.bar->setValue(0);
        r.bar->setStyleSheet(barQss);
        rowLay->addWidget(r.bar);

        lay->addWidget(r.box);
        r.box->hide();
        rows.append(r);
    }
    return box;
}

int TopPanel::fillProcSection(const QVector<ProcStat>& src, bool byUpload,
                              QVector<RowUi>& rows)
{
    QVector<ProcStat> list = src;
    std::sort(list.begin(), list.end(),
              [byUpload](const ProcStat& a, const ProcStat& b) {
                  const quint64 va = byUpload ? a.upRate : a.downRate;
                  const quint64 vb = byUpload ? b.upRate : b.downRate;
                  if (va != vb)
                      return va > vb;
                  return (byUpload ? a.upBytes : a.downBytes)
                       > (byUpload ? b.upBytes : b.downBytes);
              });

    const int n = qMin(kRowsPerSection, list.size());
    quint64 maxV = 0;
    for (int i = 0; i < n; ++i) {
        const quint64 v = byUpload ? list.at(i).upRate : list.at(i).downRate;
        if (v > maxV)
            maxV = v;
    }

    int shown = 0;
    for (int i = 0; i < n; ++i) {
        const ProcStat& s = list.at(i);
        const quint64 rate = byUpload ? s.upRate : s.downRate;
        if (rate == 0 && s.upBytes == 0 && s.downBytes == 0)
            continue;
        const RowUi& r = rows.at(shown);
        r.icon->setPixmap(iconFor(s).pixmap(16, 16));
        r.name->setText(r.name->fontMetrics().elidedText(
            s.name, Qt::ElideMiddle, kProcNameWidth));
        r.rate->setText((byUpload ? QStringLiteral("↑ ")
                                  : QStringLiteral("↓ "))
                        + formatRate(rate));
        r.bar->setValue(maxV > 0 ? int(rate * 100 / maxV) : 0);
        r.box->show();
        ++shown;
    }
    for (int i = shown; i < kRowsPerSection; ++i)
        rows.at(i).box->hide();

    return shown;
}

QIcon TopPanel::iconFor(const ProcStat& s)
{
    return ts::procIcon(s.name, s.pid, m_iconCache);
}

void TopPanel::setProcSnapshot(const QVector<ProcStat>& stats)
{
    const int upN = fillProcSection(stats, true, m_procUpRows);
    const int downN = fillProcSection(stats, false, m_procDownRows);
    const bool empty = (upN + downN) == 0;
    m_procUpSection->setVisible(!empty);
    m_procDownSection->setVisible(!empty);
    m_procHint->setVisible(empty);
}

void TopPanel::setTotals(quint64 downRate)
{
    m_totalLabel->setText(QStringLiteral("↓ %1").arg(formatRate(downRate)));
}

// 顶部摘要行：今日累计下载 / 上传。
// 由 FloatWidget 每 10 秒灌一次（不跟刷新节拍，避免每拍去查 SQLite）。
// 峰值速率不在这里显示 —— 它在详情弹窗顶部「上传总速度」那一行。
void TopPanel::setUsage(quint64 todayDown, quint64 todayUp)
{
    m_summaryLabel->setText(QStringLiteral("今日 ↓%1 ↑%2")
                              .arg(formatBytes(todayDown), formatBytes(todayUp)));
}
