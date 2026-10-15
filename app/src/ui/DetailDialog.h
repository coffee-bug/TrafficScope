#pragma once
#include <QDate>
#include <QDialog>
#include <QHash>
#include <QIcon>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QVector>
#include "Charts.h"
#include "core/ProcTraffic.h"
#include "core/StatsDb.h"
#include "core/TrafficStore.h"

class QEvent;
class QHideEvent;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QTabWidget;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class RangePopup;
namespace ts { class TipCard; }

// 表头排序状态。col < 0 表示还没点过表头（沿用数据自带的"总量降序"）；
// cols = 可排序的列数（"操作"这类按钮列不参与排序）。
struct SortState {
    int col = -1;
    Qt::SortOrder order = Qt::DescendingOrder;
    int cols = 0;
};

// 应用页的一行：
//   Proc = 进程行（按名字聚合）；Pid = 展开后该进程名下**有已建立连接**的 PID 明细行；
//   Note = 该进程当前没有任何已建立连接时的提示行。
struct RowPlan {
    enum Kind { Proc, Pid, Note };
    Kind kind = Proc;
    int procIdx = -1;       // m_current 下标
    quint64 pid = 0;        // kind == Pid
    ProcPidStat pidStat;    // kind == Pid
    bool open = false;      // kind == Proc：是否画成展开态（▾）
};

// 网站页的一行：Site = 网站行；Member = 展开后组成该网站的域名 / IP 明细行。
struct SiteRow {
    enum Kind { Site, Member };
    Kind kind = Site;
    int siteIdx = -1;       // m_sites 下标
    SiteMember member;      // kind == Member
    bool open = false;      // kind == Site：是否画成展开态（▾）
};

// 每个表格页一个搜索框，只过滤本页表格
struct SearchBox {
    QLineEdit* edit = nullptr;
    QLabel* hint = nullptr;   // 「显示 N / M 项」计数，仅在关键字非空时显示
    bool active() const;      // 关键字是否非空
    QString filter() const;   // 规范化后的关键字，无关键字时为空串
};

// 流量监控详情弹窗：
// 顶部下载/上传总速度；「应用」页为进程表（速率/累计/连接数/打开位置），
// 「域名」页为域名表（实时速率 + 累计流量），
// 「网站」页为网站表（把同一站点的多个域名 / CDN 聚合起来，可下钻到组成明细），
// 「历史累计」页读 SQLite：点日期框弹出深色日历（底部带今天/近3天/近7天/近30天/全部
// 快捷与确定按钮），支持进程 / 域名 / 网站三个维度。
// 窗口是无边框圆角卡片 + 自绘标题栏，边缘拖拽由本类自己实现（见 edgeAt）。
// 「限速」需要 WFP 驱动层，未实现。
class DetailDialog : public QDialog {
    Q_OBJECT
public:
    explicit DetailDialog(QWidget* parent = nullptr);

protected:
    // 圆角卡片 + 柔和外阴影：透明顶层窗口的 QSS 背景不渲染，必须手绘
    void paintEvent(QPaintEvent*) override;
    // 弹窗被复用（不随关闭销毁），每次显示都要重新查一次历史
    void showEvent(QShowEvent*) override;
    // ⓘ 悬浮气泡是独立顶层窗口，弹窗收起时必须一并收起
    void hideEvent(QHideEvent*) override;
    // 无边框窗口没有系统 resize 边框：按窗口四周阴影区做命中测试，自己拖拽调整大小
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    // 「已复制」轻提示是浮在卡片上的子控件，窗口尺寸变了要跟着回到正中
    void resizeEvent(QResizeEvent*) override;
    // hover 事件沿祖先链传播，所以鼠标在表格等子控件上方移动时也能刷新/清除 resize 光标
    // （QWidget::mouseMoveEvent 只在鼠标位于窗口自身那圈 kEdgeBand 内才会送到这里）
    bool event(QEvent*) override;
    // 搜索框按 Esc：有字先清空，空了才关窗
    bool eventFilter(QObject* watched, QEvent* e) override;

private slots:
    // sampleCurve：只有定时节拍该给顶部曲线补采样点（默认 true）。
    // 搜索 / 排序 / 展开这类交互只重建表格，传 false，别往曲线里塞点
    void refresh(bool sampleCurve = true);
    void refreshHistory();

private:
    void restyle();   // 主题切换：重新 setStyleSheet / setPalette / 重绘自绘部分
    // 在页面工具行左侧放搜索框 + 计数标签；history=true 仅重填历史表，
    // 且不建计数标签（历史页的计数并进下面的区间摘要）
    void addSearchBox(QWidget* parent, QHBoxLayout* lay, SearchBox& box,
                      const QString& placeholder, bool history);
    void updateSearchHint(const SearchBox& box, int shown, int total);
    void updateSiteExtHint();   // 网站页：没检测到浏览器扩展时的提示
    void fillHistoryTable();   // 按 m_histRows + 当前关键字重填历史表
    void updateHistChart();    // 按 m_histSeries + 当前关键字重算趋势柱状图（不查库）
    QWidget* makeHistoryPage();
    QIcon iconFor(const ProcStat& s);
    void setHistoryRange(int days);   // 0=今日，3/7/30=近N天，-1=自定义，-2=全部
    void setHistoryDim(int dim);      // 0=应用 1=域名 2=网站
    void exportTable(QTableWidget* t, const QString& defaultName);   // 导出当前表为 CSV
    // 四张表的右键复制
    void attachCopyMenu(QTableWidget* t);
    void showCellMenu(QTableWidget* t, const QPoint& pos);
    QString cellText(QTableWidget* t, int row, int col) const;   // 单元格真正的显示文本
    QString rowText(QTableWidget* t, int row) const;             // 整行（Tab 分隔，可贴进表格）
    void copyToClipboard(const QString& text);
    void showCopyToast();
    void placeCopyToast();
    void onRangePicked(const QDate& d);
    int edgeAt(const QPoint& pos) const;      // 命中的边缘（kEdge* 位掩码，0=不在边缘）
    void updateEdgeCursor(int edges);         // 按命中边缘切换鼠标光标
    void connectSort(QTableWidget* t, SortState& st);          // 表头点击 → 排序状态
    void onSortClicked(QTableWidget* t, SortState& st, int col);
    // 应用页：点进程行展开/收起它的连接明细子行
    void onProcRowClicked(QTableWidgetItem* item);
    void toggleRow(int row);
    // 网站页：点网站行展开/收起组成它的域名 / IP 明细
    void onSiteRowClicked(QTableWidgetItem* item);
    void fillSiteRow(int row, const SiteStat& s, bool expanded);
    void fillSiteMemberRow(int row, const SiteMember& m);
    void fillProcRow(int row, const ProcStat& s, bool expanded);
    void fillPidRow(int row, const RowPlan& p);      // 「进程ID: xxx」明细行
    void fillNoteRow(int row, const QString& text);  // 「暂无已建立的连接」这类提示行
    // 结束进程（单个 PID 或同名全部），均带二次确认
    void onKillPid(quint64 pid, const QString& name);
    void onKillProcess(const ProcStat& s);

    int m_resizeEdges = 0;    // 按下时命中的边缘
    QPoint m_pressGlobal;     // 按下时的全局坐标
    QRect m_pressGeom;        // 按下时的窗口几何

    QLabel* m_totalDown = nullptr;
    QLabel* m_totalUp = nullptr;
    QLabel* m_peakLabel = nullptr;   // 峰值速率（跟在「上传总速度」后面）
    ts::TipCard* m_tipCard = nullptr;   // ⓘ 图标的悬浮气泡（应用/域名页共用一个）
    QLabel* m_toast = nullptr;          // 「已复制」轻提示
    QTimer* m_toastTimer = nullptr;
    QTabWidget* m_tabs = nullptr;
    QTableWidget* m_procTable = nullptr;
    QTableWidget* m_domainTable = nullptr;
    QTableWidget* m_siteTable = nullptr;
    QLabel* m_siteExtHint = nullptr;   // 网站页工具行中间的「没检测到扩展」提示
    QTableWidget* m_histTable = nullptr;
    ts::SpeedGraph* m_graph = nullptr;        // 顶部实时速率曲线
    ts::HistoryChart* m_histChart = nullptr;  // 历史页趋势柱状图
    QLabel* m_histSummary = nullptr;
    QLabel* m_histDown = nullptr;    // 历史页区间汇总
    QLabel* m_histUp = nullptr;
    QPushButton* m_fromBtn = nullptr;   // 显示区间起止（点击弹日历）
    QPushButton* m_toBtn = nullptr;
    RangePopup* m_popup = nullptr;
    QDate m_customFrom;
    QDate m_customTo;
    bool m_pickingFrom = true;
    QPushButton* m_histAppBtn = nullptr;
    QPushButton* m_histDomainBtn = nullptr;
    QPushButton* m_histSiteBtn = nullptr;
    QWidget* m_dimCorner = nullptr;
    // 四个表的排序状态（cols 排除"操作"按钮列：应用 6 / 域名 5 / 网站 5 / 历史 5）
    SortState m_procSort{-1, Qt::DescendingOrder, 6};
    SortState m_domainSort{-1, Qt::DescendingOrder, 5};
    SortState m_siteSort{-1, Qt::DescendingOrder, 5};
    SortState m_histSort{-1, Qt::DescendingOrder, 5};

    QVector<RowPlan> m_plan;            // 应用页当前的行布局（进程行 + 展开出来的子行）
    QSet<QString> m_expanded;           // 已展开到 PID 明细的进程名（按名字聚合，故用名字记账）
    QVector<SiteRow> m_sitePlan;        // 网站页当前的行布局（网站行 + 展开出来的域名明细）
    QSet<QString> m_siteExpanded;       // 已展开到域名明细的网站名
    // 四个搜索框，各自只过滤本页
    SearchBox m_procSearch;
    SearchBox m_domainSearch;
    SearchBox m_siteSearch;
    SearchBox m_histSearch;
    QVector<StatsDb::Row> m_histRows;   // 历史页最近一次查询结果
    QVector<StatsDb::NamePoint> m_histSeries;   // 同一区间的「桶 × 名称」明细
    QString m_histRangeText;            // 区间摘要（不含搜索计数）
    bool m_histByDay = false;           // 趋势柱状图是否按天分桶
    QTimer* m_timer = nullptr;
    int m_histTick = 0;
    int m_histRange = 0;      // 0=今日，3/7/30，-1=自定义，-2=全部
    int m_histDim = 0;        // 0=应用 1=域名 2=网站
    QVector<ProcStat> m_current;
    QVector<SiteStat> m_sites;
    QHash<QString, QIcon> m_iconCache;
};
