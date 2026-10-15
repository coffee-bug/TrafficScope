#pragma once
#include <QString>
#include <QVector>
#include <QtGlobal>

// 历史记录的存储粒度：同一份流量按多大的时间桶合并成一行。
// 按天存的行数约为按小时的 1/24（域名维度实测 388 行/小时 → 约 16 行/天），
// 代价是查"某几个小时"时精度变差。
//
// 切换粒度**只影响之后落盘的数据**：小时粒度继续写原有的 hourly_* 表，
// 天粒度写 bucket_* 表，已存在的行不迁移、不改写、不删除。
enum class Granularity : int {
    Hour = 1,
    Day  = 2
};

// 历史流量统计库（SQLite）：按小时记录每个进程 / 每个域名的上下行增量。
// 同一小时同一名称重复写入时增量累加（UPSERT）。
// 天粒度另存 bucket_* 表（带 start_ts / end_ts），查询时两张表合并。
// 数据库文件位于用户数据目录的 data/traffic.db
// （%LOCALAPPDATA%\TrafficScope\data，见 core/Settings.h），
// 所有操作均在主线程调用。
struct sqlite3;

class StatsDb {
public:
    static StatsDb& instance();

    struct Row {
        QString name;
        quint64 up = 0;
        quint64 down = 0;
    };

    bool open(QString* err = nullptr);
    void close();
    bool isOpen() const { return m_ok; }

    // 小时粒度：hourTs 为整点时间戳（秒）
    void addApp(qint64 hourTs, const QString& name, quint64 up, quint64 down);
    void addDomain(qint64 hourTs, const QString& name, quint64 up, quint64 down);
    void addSite(qint64 hourTs, const QString& name, quint64 up, quint64 down);

    // 天粒度：[startTs, endTs) 为该桶覆盖的时间区间
    void addAppBucket(qint64 startTs, qint64 endTs, const QString& name,
                      quint64 up, quint64 down);
    void addDomainBucket(qint64 startTs, qint64 endTs, const QString& name,
                         quint64 up, quint64 down);
    void addSiteBucket(qint64 startTs, qint64 endTs, const QString& name,
                       quint64 up, quint64 down);

    // 区间 [fromTs, toTs) 内按名称聚合，按总流量降序。
    // 两张表合并统计：命中条件是"桶的时间区间与查询区间相交"——粗粒度桶的起点
    // 往往早于查询区间，只按起点过滤会把它整段漏掉。
    QVector<Row> queryApps(qint64 fromTs, qint64 toTs, int limit = 500);
    QVector<Row> queryDomains(qint64 fromTs, qint64 toTs, int limit = 500);
    QVector<Row> querySites(qint64 fromTs, qint64 toTs, int limit = 500);

    // 区间 [fromTs, toTs) 的**总流量**（不按名称分组）。
    // 给悬停面板的「今日用量」摘要用 —— 只要一个数，不需要明细。
    struct Totals {
        quint64 up = 0;
        quint64 down = 0;
        quint64 total() const { return up + down; }
    };
    Totals totals(qint64 fromTs, qint64 toTs, bool byApp);

    // 区间内按 bucketSec 分桶的时间序列（升序），给历史趋势图用。
    struct Point {
        qint64 ts = 0;        // 桶起点（本地时间对齐后的整桶起点）
        quint64 up = 0;
        quint64 down = 0;
    };
    // 同样的分桶，但带上名称：调用方按当前搜索关键字挑名称，自己合并成 Point。
    struct NamePoint {
        QString name;
        qint64 ts = 0;
        quint64 up = 0;
        quint64 down = 0;
    };
    // 注意 **utcOffsetSec 必须传**：库里的 hour_ts / start_ts 是**本地时间**对齐的
    // （见 StatsRecorder::hourStart / bucketRange）。直接对 epoch 整除会按 **UTC** 分桶，
    // 在 UTC+8 下每根柱子会偏移 8 小时（天视图尤其明显）。
    // dim：0=应用 1=域名 2=网站（对应三对表）
    QVector<NamePoint> seriesByName(qint64 fromTs, qint64 toTs, int dim, qint64 bucketSec,
                                    qint64 utcOffsetSec = 0);

    qint64 earliestTs();          // 最早记录时间；无记录返回 0
    // 按维度删除早于给定时间的记录；某维度传 <= 0 表示跳过不清理
    // （注意：ini 里"永久保留"记的是 -1，由 StatsRecorder 换算成这里的 0）
    // site 与 domain 共用同一份保留天数（数据同源，由 StatsRecorder 传同一个值）
    void prune(qint64 appBeforeTs, qint64 domainBeforeTs, qint64 siteBeforeTs);

    // 清空历史：app / domain 两个维度分别开关；清完顺手 VACUUM 把文件空间还回去
    // （只 DELETE 不 VACUUM 的话，页只回到 freelist，traffic.db 不会变小）
    // 网站表跟域名一起清（同一份流量的另一个切片，单独留一半只会对不上）
    void clear(bool app, bool domain);

private:
    StatsDb() = default;

    bool ensureTables();
    void addRow(const QString& table, const QString& col, qint64 hourTs,
                const QString& name, quint64 up, quint64 down);
    void addBucketRow(const QString& table, const QString& col, qint64 startTs,
                      qint64 endTs, const QString& name, quint64 up, quint64 down);
    // hourTable / bucketTable 是一对（如 hourly_app / bucket_app）
    QVector<Row> queryBy(const QString& hourTable, const QString& bucketTable,
                         const QString& col, qint64 fromTs, qint64 toTs, int limit);

    bool m_ok = false;
    QString m_path;
    sqlite3* m_db = nullptr;
};
