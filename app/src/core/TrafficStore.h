#pragma once
#include "IpKey.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <atomic>
#include <cstdint>
#include <functional>

struct DomainStat {
    QString domain;
    quint64 upBytes = 0;      // 累计上传
    quint64 downBytes = 0;    // 累计下载
    quint64 upRate = 0;       // 实时上传速率 bytes/s
    quint64 downRate = 0;
};

// 「网站」维度的一行：一个网站 = 注册域名 + 归到它名下的 CDN 域名 / IP。
// 与 DomainStat 的关系：域名是"抓包能看到的最小单位"，网站是把同一站点的
// 多个域名（www / img / CDN 别名）合起来的聚合视图。
struct SiteStat {
    QString site;
    quint64 upBytes = 0;
    quint64 downBytes = 0;
    quint64 upRate = 0;
    quint64 downRate = 0;
    bool shared = false;      // 含被多个网站共用的 CDN 域名，归属可能不准
};

// 网站下钻的一行：某个域名（或裸 IP）为这个网站贡献了多少流量
struct SiteMember {
    QString name;
    quint64 upBytes = 0;
    quint64 downBytes = 0;
    quint64 upRate = 0;
    quint64 downRate = 0;
};

// 采样增量：供历史统计按小时落盘
struct StatDelta {
    QString name;
    quint64 up = 0;
    quint64 down = 0;
};

struct ProcStat {
    QString name;             // 进程名 "msedge.exe"（同名多进程已聚合）
    QString desc;             // exe 的 FileDescription，UI 第二行；取不到为空
    quint64 pid = 0;          // 该名字下流量最大的 PID（用于取图标/打开位置）
    quint64 upBytes = 0;
    quint64 downBytes = 0;
    quint64 upRate = 0;
    quint64 downRate = 0;
    int connCount = 0;        // 聚合的 TCP ESTABLISHED 连接数
};

// 同一个进程名下的单个 PID（详情弹窗"展开"后按 PID 展示的一行）。
// 与 ProcStat 的关系：ProcStat 是"同名进程聚合"，这里是聚合前的那一份。
struct ProcPidStat {
    quint64 pid = 0;
    QString desc;             // 该 PID 的 FileDescription（第二行描述），可能为空
    quint64 upBytes = 0;
    quint64 downBytes = 0;
    quint64 upRate = 0;
    quint64 downRate = 0;
    int connCount = 0;        // 该 PID 的 TCP ESTABLISHED 连接数
};

inline QString formatBytes(quint64 b)
{
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    const int prec = (u == 0 || v >= 100.0) ? 0 : (v >= 10.0 ? 1 : 2);
    return QString::number(v, 'f', prec) + QLatin1String(units[u]);
}

// 条数的中式进位显示：1234 → "1234"，1.2万 → "1.2万"，3.4亿 → "3.4亿"。
// 跳过"千"（1234 写成 1.2千 反而难读），一万起步才进位。
inline QString formatCount(quint64 n)
{
    if (n >= 100000000ULL) {
        const double v = double(n) / 100000000.0;
        return QString::number(v, 'f', v >= 100.0 ? 0 : 1) + QLatin1String("亿");
    }
    if (n >= 10000ULL) {
        const double v = double(n) / 10000.0;
        return QString::number(v, 'f', v >= 100.0 ? 0 : 1) + QLatin1String("万");
    }
    return QString::number(n);
}

// 速率显示单位：0=字节（B/s，默认，与早期版本一致）；1=比特（bps，运营商带宽口径）。
// 由设置项 [Float] unit 驱动，全局唯一 —— 所有 formatRate() 调用点自动跟随，不必逐个传参。
// 这也是把开关放在这里而不是 Settings.h 的原因：**格式化只有这一个出口**。
inline int& rateUnitModeStorage()
{
    static int m = 0;
    return m;
}

inline int rateUnitMode() { return rateUnitModeStorage(); }
inline void setRateUnitMode(int m) { rateUnitModeStorage() = (m == 1 ? 1 : 0); }

// 比特模式：1 B = 8 bit，按 **1000 进制**进位（与宽带标称口径一致，如 100Mbps）。
// 注意与字节模式的 1024 进制不同，两者数字不会完全相同（1000 vs 1024）。
inline QString formatRateBits(quint64 bytesPerSec)
{
    static const char* units[] = {"bps", "Kbps", "Mbps", "Gbps", "Tbps"};
    double v = double(bytesPerSec) * 8.0;
    int u = 0;
    while (v >= 1000.0 && u < 4) {
        v /= 1000.0;
        ++u;
    }
    const int prec = (u == 0 || v >= 100.0) ? 0 : (v >= 10.0 ? 1 : 2);
    return QString::number(v, 'f', prec) + QLatin1String(units[u]);
}

inline QString formatRate(quint64 bytesPerSec)
{
    return rateUnitMode() == 1 ? formatRateBits(bytesPerSec)
                               : formatBytes(bytesPerSec) + QStringLiteral("/s");
}

// 线程安全的流量聚合表：抓包线程写入，UI 线程按 refreshMs（默认 1s）定时快照。
class TrafficStore {
public:
    static TrafficStore& instance();

    void addBytes(const IpKey& remoteIp, bool upload, quint64 bytes);
    void addProcBytes(quint64 pid, bool upload, quint64 bytes);

    // 计算快照并返回 Top-N（按域名聚合，DNS/SNI 回填）。
    // sortMode: 0=总流量 1=上传 2=下载
    QVector<DomainStat> snapshot(int topN = 20, int sortMode = 0);

    // 按网站聚合的快照 Top-N。与 snapshot() 共享同一节拍（同一份 m_byIp、同一个
    // 差分基准），所以**先调 snapshot() 再调它就不会重复推进基准**。
    QVector<SiteStat> siteSnapshot(int topN = 20);

    // 某个网站下的域名 / IP 明细（下钻用，按总流量降序）。
    // 由 siteSnapshot()（或 snapshot()）那一拍填好，缓存为空时才补一次快照。
    QVector<SiteMember> siteMembers(const QString& site);

    // 按进程名聚合的快照 Top-N（pid=0 归入"未知进程"）
    QVector<ProcStat> procSnapshot(int topN = 20);

    // 某个进程名下按 PID 拆开的明细（详情弹窗展开一行时用）。
    // 数据由 procSnapshot() 在同一次差分里顺带算好并缓存，所以
    // **必须在 procSnapshot() 之后调用**（缓存里没有就返回空）。
    QVector<ProcPidStat> procDetail(const QString& name);



    // 自上次调用以来的字节增量（已按进程名/域名聚合），供历史统计按小时落盘。
    // 与 snapshot/procSnapshot 的速率差分互不干扰，使用独立的采样基准。
    QVector<StatDelta> takeProcSample();
    QVector<StatDelta> takeDomainSample();
    // 网站维度的采样：与 takeDomainSample 同样的增量、不同的聚合键
    // （名字是"当时归属到的网站"，归不到的以裸 IP 名落库 —— 落库后不再随归属变化）
    QVector<StatDelta> takeSiteSample();

    // 最近一次快照的全局速率
    void totals(quint64& upRate, quint64& downRate);

    // 本次运行（或上次 reset）以来的峰值速率
    // 不加 const：实现里要锁 m_mutex（QMutex 不是 mutable，const 方法里锁不了）
    void peaks(quint64& upRate, quint64& downRate);

    // 是否统计"未归因流量"（pid == 0 的流量），抓包线程每包都会读，用 atomic。
    // 关掉后：抓包不再把它计入任何维度、进程榜/详情不再展示，历史页也过滤掉这一行；
    // **已落盘的历史数据保留**（只是不再显示，不删除）。
    bool countsUnattributed() const { return m_countUnattributed.load(); }
    void setCountsUnattributed(bool on) { m_countUnattributed.store(on); }

    void reset();

private:
    TrafficStore() = default;

    struct Entry {
        quint64 up = 0;
        quint64 down = 0;
        qint64 lastSeenMs = 0;
    };

    // 采样公共骨架：取 m_byIp 的增量，按 keyOf(ip) 聚合。
    // 域名维度与网站维度只在"聚合键"上不同，共用这一份差分逻辑。
    QVector<StatDelta> sampleByIp(QHash<IpKey, Entry>& baseline,
                                  const std::function<QString(const IpKey&)>& keyOf);
    // 显示名回填：DNS 优先，SNI 兜底，最后退化为裸 IP 字符串
    QString resolveHost(const IpKey& ip) const;

    QMutex m_mutex;
    QHash<IpKey, Entry> m_byIp;              // 远端IP（v4/v6 统一）-> 累计字节
    QHash<IpKey, Entry> m_prevSnapshot;      // 上次快照，用于差分算速率
    qint64 m_lastSnapshotMs = 0;
    QHash<quint64, Entry> m_byPid;           // 进程PID -> 累计字节
    QHash<quint64, Entry> m_prevPidSnapshot;
    qint64 m_lastProcMs = 0;
    QVector<ProcStat> m_procCache;           // 500ms 内的调用复用，避免差分被摊薄
    QVector<DomainStat> m_domainCache;       // 同上（浮框与详情弹窗共享同一节拍）
    QVector<SiteStat> m_siteCache;           // 网站维度快照（与上面同一拍算好）
    QHash<QString, QVector<SiteMember>> m_siteMembers;   // 网站 -> 组成它的域名/IP
    QHash<QString, QVector<ProcPidStat>> m_pidDetail;   // 进程名 -> 各 PID 明细（同上节拍）
    QHash<IpKey, Entry> m_samplePrevIp;      // 上次落盘采样时的累计值（计算增量用）
    QHash<IpKey, Entry> m_samplePrevIpSite;  // 网站维度采样的独立基准（互不干扰）
    QHash<quint64, Entry> m_samplePrevPid;
    // 设置项：是否统计未归因流量。默认 false（= 设置里「不再统计未归因流量」勾选），
    // 启动时 main.cpp 会用 settings.ini 的真实值覆盖。
    std::atomic<bool> m_countUnattributed{false};
    quint64 m_totalUpRate = 0;
    quint64 m_totalDownRate = 0;
    quint64 m_peakUpRate = 0;      // 峰值速率（reset 时清零）
    quint64 m_peakDownRate = 0;
};
