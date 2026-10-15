#pragma once
#include "StatsDb.h"

#include <QObject>
#include <QTimer>
#include <QtGlobal>

class QDateTime;

// 周期性把流量增量落盘到 SQLite，供详情弹窗的「历史累计」查询。
// 采样周期 60s，增量归入采样时刻所在的时间桶（粒度由用户设置：小时/天，
// 默认小时）；每天执行一次过期清理。
// 保留天数由用户在「设置」里分别配置（应用 / 域名两个维度），改动即时生效。
class StatsRecorder : public QObject {
    Q_OBJECT
public:
    static StatsRecorder& instance();

    bool start(QString* err = nullptr);
    void stop();   // 补记最后一分钟并关闭数据库

    // 按需落盘：UI 查看历史前调用，保证读到的是最新增量。
    // 不改变 60s 的周期采样，也不额外占用内存（只是把当前增量提前写入同一小时桶）。
    void flushNow();

    // 历史保留天数：**-1 = 永久保留（不清理）**，正整数 = 保留天数。
    // 域名维度是"每个域名每小时一行"，条目数比应用维度高一个数量级
    // （实测 388 行/小时 vs 31 行/小时），所以两者分开配置；调小时立即
    // 按新值清理一次。（ini 里手写的 0 或其它非法值一律回落到默认 90 天，
    // 避免出现"0 到底是不保留还是永久"的歧义。）
    int appRetainDays() const { return m_appRetainDays; }
    int domainRetainDays() const { return m_domainRetainDays; }
    void setAppRetainDays(int days);
    void setDomainRetainDays(int days);

    // 记录粒度：只影响设置生效**之后**落盘的数据，已有记录保持原粒度不动。
    // 切换时会先把待落盘的增量按旧粒度提交，避免同一批增量混进新桶。
    Granularity granularity() const { return m_granularity; }
    void setGranularity(Granularity g);

    // ini 里"永久保留"的记法。以前用的是 0，容易跟"保留 0 天"混，改成 -1。
    static constexpr int kRetainForever = -1;

    // 清空历史（用户点「清空历史记录」）：先把当前增量落盘保证清得干净，
    // 清完再把采样基准推进一次 —— 这样库里只剩"清空之后"发生的流量。
    // 只影响已落盘的历史，不动 TrafficStore 里的实时统计。
    void clearHistory(bool app, bool domain);

private slots:
    void onSample();

private:
    StatsRecorder() = default;

    static constexpr int kDefaultRetainDays = 90;

    // 采样时刻 dt 所属时间桶的 [start, end)（秒级时间戳、本地时区）：
    // 小时=整点、天=本地 0 点
    static void bucketRange(const QDateTime& dt, Granularity g,
                            qint64& start, qint64& end);
    static qint64 hourStart(const QDateTime& dt);
    static int readDays(const char* key);          // 读 settings.ini，非法值回落默认
    static int normalizeRetainDays(int days);      // 收敛成"天数 >= 1"或 kRetainForever
    static Granularity normalizeGranularity(int g);   // ini 里的裸整数 -> 枚举（非法值=小时）
    static void saveDays(const char* key, int days);
    void pruneNow();                               // 按当前保留天数清理过期记录

    QTimer m_timer;
    qint64 m_lastPruneDay = 0;
    int m_appRetainDays = kDefaultRetainDays;
    int m_domainRetainDays = kDefaultRetainDays;
    Granularity m_granularity = Granularity::Hour;
};
