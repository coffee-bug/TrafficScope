#pragma once
#include <QHash>
#include <QIcon>
#include <QString>
#include <QVector>
#include <QWidget>
#include "core/TrafficStore.h"

class QLabel;
class QPaintEvent;
class QProgressBar;
class QPushButton;

// 悬浮面板：只展示应用（进程）实时榜单 + 底部「查看详情」。
// 域名榜与累计明细已移入详情弹窗（DetailDialog），面板保持轻量。
class TopPanel : public QWidget {
    Q_OBJECT
public:
    explicit TopPanel(QWidget* parent = nullptr);

    void setProcSnapshot(const QVector<ProcStat>& stats);
    void setTotals(quint64 downRate);   // 标题栏只展示下载总速度

    // 顶部摘要行：今日累计下载 / 上传。
    // 由 FloatWidget 每 10 秒灌一次（不跟刷新节拍，避免每拍去查 SQLite）。
    // 峰值速率不在这里显示 —— 它在详情弹窗顶部「上传总速度」那一行。
    void setUsage(quint64 todayDown, quint64 todayUp);

signals:
    void detailRequested();   // 点击底部「查看详情」

protected:
    void paintEvent(QPaintEvent*) override;

private:
    struct RowUi {
        QWidget* box = nullptr;
        QLabel* icon = nullptr;
        QLabel* name = nullptr;
        QLabel* rate = nullptr;
        QProgressBar* bar = nullptr;
    };

    QWidget* makeSection(const QString& title, const QColor& accent,
                         const QString& rateObj, QVector<RowUi>& rows);
    int fillProcSection(const QVector<ProcStat>& src, bool byUpload,
                        QVector<RowUi>& rows);
    QIcon iconFor(const ProcStat& s);
    void restyle();   // 主题切换：重新套 QSS（含每行的占比条配色）

    QLabel* m_totalLabel = nullptr;
    QLabel* m_summaryLabel = nullptr;   // 今日 ↓x ↑y
    QLabel* m_procHint = nullptr;
    QWidget* m_procUpSection = nullptr;
    QWidget* m_procDownSection = nullptr;
    QVector<RowUi> m_procUpRows;
    QVector<RowUi> m_procDownRows;
    QHash<QString, QIcon> m_iconCache;   // 进程名 -> 图标
};
