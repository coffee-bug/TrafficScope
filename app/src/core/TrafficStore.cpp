#include "TrafficStore.h"

#include "DnsSniffer.h"
#include "ProcTraffic.h"
#include "SiteResolver.h"
#include "TlsSniParser.h"

#include <QDateTime>
#include <algorithm>

namespace {

// 远端 IP / PID 无流量后的回收时间：防止长期运行内存膨胀
constexpr qint64 kEntryTtlMs = 2 * 3600 * 1000;
// 快照结果缓存窗口：浮框与详情弹窗共享同一节拍，避免重复推进差分基准把速率摊薄
constexpr qint64 kSnapshotCacheMs = 500;
// 间隔小于此值的两拍不参与峰值（启动瞬间 dt 很小，会算出虚高值）
constexpr double kMinDtSec = 0.05;

}  // namespace

TrafficStore& TrafficStore::instance()
{
    static TrafficStore s;
    return s;
}

void TrafficStore::addBytes(const IpKey& remoteIp, bool upload, quint64 bytes)
{
    QMutexLocker lock(&m_mutex);
    Entry& e = m_byIp[remoteIp];
    if (upload)
        e.up += bytes;
    else
        e.down += bytes;
    e.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
}

void TrafficStore::addProcBytes(quint64 pid, bool upload, quint64 bytes)
{
    QMutexLocker lock(&m_mutex);
    Entry& e = m_byPid[pid];
    if (upload)
        e.up += bytes;
    else
        e.down += bytes;
    e.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
}

QVector<DomainStat> TrafficStore::snapshot(int topN, int sortMode)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 同 procSnapshot：500ms 内复用缓存，避免多个消费者重复推进差分基准。
    // 缓存命中时 sortMode 沿用首次调用的选择（调用方目前均使用 0）。
    if (m_lastSnapshotMs > 0 && now - m_lastSnapshotMs < kSnapshotCacheMs)
        return m_domainCache.mid(0, topN);

    QHash<IpKey, Entry> cur;
    QHash<IpKey, Entry> prev;
    {
        QMutexLocker lock(&m_mutex);
        // 清理 2 小时无流量的远端 IP，防止长期运行内存膨胀
        for (auto it = m_byIp.begin(); it != m_byIp.end();) {
            if (now - it->lastSeenMs > kEntryTtlMs)
                it = m_byIp.erase(it);
            else
                ++it;
        }
        cur = m_byIp;
        prev = m_prevSnapshot;
        m_prevSnapshot = cur;
    }

    const double dt = (m_lastSnapshotMs > 0) ? double(now - m_lastSnapshotMs) / 1000.0 : 0.0;
    m_lastSnapshotMs = now;

    quint64 totalUp = 0;
    quint64 totalDown = 0;
    QHash<QString, DomainStat> byDomain;
    // 网站维度与域名维度在同一遍循环里算：都是 m_byIp 的聚合视图，
    // 分开算要多走一遍几百~几千条的表，且两边速率不是同一拍的差分。
    QHash<QString, SiteStat> bySite;
    QHash<QString, QHash<QString, SiteMember>> memberBuf;   // 网站 -> 域名 -> 明细

    for (auto it = cur.constBegin(); it != cur.constEnd(); ++it) {
        const IpKey ip = it.key();
        const Entry& e = it.value();

        quint64 upRate = 0;
        quint64 downRate = 0;
        if (dt > kMinDtSec) {
            const Entry pe = prev.value(ip);
            if (e.up > pe.up)
                upRate = quint64(double(e.up - pe.up) / dt);
            if (e.down > pe.down)
                downRate = quint64(double(e.down - pe.down) / dt);
        }
        totalUp += upRate;
        totalDown += downRate;

        const QString domain = resolveHost(ip);

        DomainStat& s = byDomain[domain];
        s.domain = domain;
        s.upBytes += e.up;
        s.downBytes += e.down;
        s.upRate += upRate;
        s.downRate += downRate;

        // 网站归属：只认浏览器扩展回传的那一条，没装上就没有网站行
        const ts::SiteInfo si = ts::SiteResolver::instance().resolve(ip, domain);
        if (si.source != ts::SiteSource::Browser)
            continue;

        SiteStat& st = bySite[si.site];
        st.site = si.site;
        st.upBytes += e.up;
        st.downBytes += e.down;
        st.upRate += upRate;
        st.downRate += downRate;
        if (si.shared)
            st.shared = true;

        SiteMember& mem = memberBuf[si.site][domain];
        mem.name = domain;
        mem.upBytes += e.up;
        mem.downBytes += e.down;
        mem.upRate += upRate;
        mem.downRate += downRate;
    }

    // 网站榜：把同一次差分里攒的明细整理成缓存（下钻直接读，不再重算）
    QVector<SiteStat> siteList;
    siteList.reserve(int(bySite.size()));
    m_siteMembers.clear();
    for (auto it = bySite.constBegin(); it != bySite.constEnd(); ++it) {
        SiteStat s = it.value();
        const QHash<QString, SiteMember>& mm = memberBuf.value(it.key());
        QVector<SiteMember> v;
        v.reserve(int(mm.size()));
        for (auto jt = mm.constBegin(); jt != mm.constEnd(); ++jt)
            v.append(jt.value());
        std::stable_sort(v.begin(), v.end(),
                         [](const SiteMember& a, const SiteMember& b) {
                             return a.upBytes + a.downBytes > b.upBytes + b.downBytes;
                         });
        m_siteMembers.insert(it.key(), v);
        siteList.append(s);
    }
    std::stable_sort(siteList.begin(), siteList.end(),
                     [](const SiteStat& a, const SiteStat& b) {
                         return a.upBytes + a.downBytes > b.upBytes + b.downBytes;
                     });
    m_siteCache = siteList;

    {
        QMutexLocker lock(&m_mutex);
        m_totalUpRate = totalUp;
        m_totalDownRate = totalDown;
        // 峰值只在差分有效时推进：dt 太小的那一拍速率不参与（避免启动瞬间虚高）
        if (dt > kMinDtSec) {
            if (totalUp > m_peakUpRate)
                m_peakUpRate = totalUp;
            if (totalDown > m_peakDownRate)
                m_peakDownRate = totalDown;
        }
    }

    QVector<DomainStat> list;
    list.reserve(int(byDomain.size()));
    for (auto it = byDomain.constBegin(); it != byDomain.constEnd(); ++it)
        list.append(it.value());

    auto score = [sortMode](const DomainStat& s) -> quint64 {
        if (sortMode == 1)
            return s.upBytes;
        if (sortMode == 2)
            return s.downBytes;
        return s.upBytes + s.downBytes;
    };
    std::stable_sort(list.begin(), list.end(),
                     [&score](const DomainStat& a, const DomainStat& b) {
                         return score(a) > score(b);
                     });
    m_domainCache = list;
    if (list.size() > topN)
        list.resize(topN);
    return list;
}

QVector<SiteStat> TrafficStore::siteSnapshot(int topN)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 与 snapshot() 同一节拍：500ms 内直接复用缓存，过期了就自己推进一拍
    // （它会顺带把 m_siteCache / m_siteMembers 填好）。
    if (m_lastSnapshotMs > 0 && now - m_lastSnapshotMs < kSnapshotCacheMs)
        return m_siteCache.mid(0, topN);

    snapshot();
    return m_siteCache.mid(0, topN);
}

QVector<SiteMember> TrafficStore::siteMembers(const QString& site)
{
    // 缓存由快照那一拍填好；没取过快照（刚启动还没数据）就先取一次，
    // snapshot 有 500ms 缓存，重复调用代价很低。
    if (m_siteMembers.isEmpty())
        snapshot(1);
    return m_siteMembers.value(site);
}

QVector<ProcStat> TrafficStore::procSnapshot(int topN)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 多个消费者（浮框榜单、详情弹窗）共享同一秒节拍：
    // 500ms 内的调用直接复用缓存，避免重复推进差分基准把速率摊薄
    if (m_lastProcMs > 0 && now - m_lastProcMs < kSnapshotCacheMs)
        return m_procCache.mid(0, topN);

    QHash<quint64, Entry> cur;
    QHash<quint64, Entry> prev;
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_byPid.begin(); it != m_byPid.end();) {
            if (now - it->lastSeenMs > kEntryTtlMs)
                it = m_byPid.erase(it);
            else
                ++it;
        }
        cur = m_byPid;
        prev = m_prevPidSnapshot;
        m_prevPidSnapshot = cur;
    }

    // 独立于域名快照的差分基准，两者在同一次 onTick 里先后调用
    const double dt = (m_lastProcMs > 0) ? double(now - m_lastProcMs) / 1000.0 : 0.0;
    m_lastProcMs = now;

    QHash<QString, ProcStat> byName;
    QHash<QString, quint64> bestBytes;   // 同名多进程（如 msedge）取流量最大的 PID 做图标
    QHash<QString, QVector<ProcPidStat>> pidDetail;   // 进程名 -> 各 PID 明细
    const bool keepUnattr = m_countUnattributed.load();
    for (auto it = cur.constBegin(); it != cur.constEnd(); ++it) {
        const quint64 pid = it.key();
        if (pid == 0 && !keepUnattr)
            continue;   // 设置项：不再展示未归因流量（库里的历史数据不动）
        const Entry& e = it.value();

        quint64 upRate = 0;
        quint64 downRate = 0;
        if (dt > kMinDtSec) {
            const Entry pe = prev.value(pid);
            if (e.up > pe.up)
                upRate = quint64(double(e.up - pe.up) / dt);
            if (e.down > pe.down)
                downRate = quint64(double(e.down - pe.down) / dt);
        }

        const QString name = ProcTraffic::instance().nameFor(pid);
        ProcStat& s = byName[name];
        s.name = name;
        if (s.desc.isEmpty())
            s.desc = ProcTraffic::instance().descFor(pid);
        const quint64 total = e.up + e.down;
        if (s.pid == 0 || total > bestBytes.value(name)) {
            s.pid = pid;
            bestBytes[name] = total;
        }
        s.upBytes += e.up;
        s.downBytes += e.down;
        s.upRate += upRate;
        s.downRate += downRate;
        s.connCount += ProcTraffic::instance().connCountFor(pid);

        // 展开用的按 PID 明细：与聚合值同一次差分，不再单独算一遍
        ProcPidStat d;
        d.pid = pid;
        d.desc = ProcTraffic::instance().descFor(pid);
        d.upBytes = e.up;
        d.downBytes = e.down;
        d.upRate = upRate;
        d.downRate = downRate;
        d.connCount = ProcTraffic::instance().connCountFor(pid);
        pidDetail[name].append(d);
    }

    // 同名下各 PID 按总量降序（展开后第一眼看到最占流量的那个）
    for (auto it = pidDetail.begin(); it != pidDetail.end(); ++it) {
        std::stable_sort(it->begin(), it->end(),
                         [](const ProcPidStat& a, const ProcPidStat& b) {
                             return a.upBytes + a.downBytes > b.upBytes + b.downBytes;
                         });
    }
    m_pidDetail = pidDetail;

    QVector<ProcStat> list;
    list.reserve(int(byName.size()));
    for (auto it = byName.constBegin(); it != byName.constEnd(); ++it)
        list.append(it.value());

    std::stable_sort(list.begin(), list.end(),
                     [](const ProcStat& a, const ProcStat& b) {
                         return a.upBytes + a.downBytes
                              > b.upBytes + b.downBytes;
                     });
    m_procCache = list;
    if (list.size() > topN)
        list.resize(topN);
    return list;
}

QVector<ProcPidStat> TrafficStore::procDetail(const QString& name)
{
    // 缓存由 procSnapshot 那一拍填好；若调用方还没取过快照（缓存为空），
    // 就先取一次（它有 500ms 缓存，重复调用代价很低）。
    if (m_pidDetail.isEmpty())
        procSnapshot(1);
    return m_pidDetail.value(name);
}

QVector<StatDelta> TrafficStore::takeProcSample()
{
    QHash<quint64, Entry> cur;
    QHash<quint64, Entry> prev;
    {
        QMutexLocker lock(&m_mutex);
        cur = m_byPid;
        prev = m_samplePrevPid;
        m_samplePrevPid = cur;
    }

    QHash<QString, StatDelta> agg;
    const bool keepUnattr = m_countUnattributed.load();
    for (auto it = cur.constBegin(); it != cur.constEnd(); ++it) {
        const quint64 pid = it.key();
        if (pid == 0 && !keepUnattr)
            continue;   // 同上：未归因流量不再落盘（已有的历史记录保留）
        const Entry pe = prev.value(pid);
        const Entry& e = it.value();
        const quint64 up = e.up > pe.up ? e.up - pe.up : 0;
        const quint64 down = e.down > pe.down ? e.down - pe.down : 0;
        if (up == 0 && down == 0)
            continue;
        const QString name = ProcTraffic::instance().nameFor(pid);
        StatDelta& d = agg[name];
        d.name = name;
        d.up += up;
        d.down += down;
    }

    QVector<StatDelta> list;
    list.reserve(int(agg.size()));
    for (auto it = agg.constBegin(); it != agg.constEnd(); ++it)
        list.append(it.value());
    return list;
}

QVector<StatDelta> TrafficStore::takeDomainSample()
{
    // 域名维度：聚合键 = 当时的显示名（DNS 优先，SNI 兜底，归不到就是裸 IP）
    return sampleByIp(m_samplePrevIp, [this](const IpKey& ip) { return resolveHost(ip); });
}

QVector<StatDelta> TrafficStore::takeSiteSample()
{
    // 与 takeDomainSample 同一份增量，不同的聚合键：存"当时归属到的网站"。
    // 归属是**落库那一刻**判断的，之后扩展/DNS 学到新映射也不会回填老数据 ——
    // 这正是它存在的意义：历史数字一旦写下就不会漂。
    return sampleByIp(m_samplePrevIpSite, [this](const IpKey& ip) {
        const QString host = resolveHost(ip);
        const ts::SiteInfo si = ts::SiteResolver::instance().resolve(ip, host);
        // 只认浏览器扩展回传的归属，其余不落库
        return si.source == ts::SiteSource::Browser ? si.site : QString();
    });
}

QVector<StatDelta> TrafficStore::sampleByIp(QHash<IpKey, Entry>& baseline,
                                            const std::function<QString(const IpKey&)>& keyOf)
{
    QHash<IpKey, Entry> cur;
    QHash<IpKey, Entry> prev;
    {
        QMutexLocker lock(&m_mutex);
        cur = m_byIp;
        prev = baseline;
        baseline = cur;
    }

    QHash<QString, StatDelta> agg;
    for (auto it = cur.constBegin(); it != cur.constEnd(); ++it) {
        const Entry pe = prev.value(it.key());
        const Entry& e = it.value();
        const quint64 up = e.up > pe.up ? e.up - pe.up : 0;
        const quint64 down = e.down > pe.down ? e.down - pe.down : 0;
        if (up == 0 && down == 0)
            continue;
        const QString key = keyOf(it.key());
        if (key.isEmpty())     // 空键 = 这个 IP 不进本维度
            continue;
        StatDelta& d = agg[key];
        d.name = key;
        d.up += up;
        d.down += down;
    }

    QVector<StatDelta> list;
    list.reserve(int(agg.size()));
    for (auto it = agg.constBegin(); it != agg.constEnd(); ++it)
        list.append(it.value());
    return list;
}

QString TrafficStore::resolveHost(const IpKey& ip) const
{
    // 显示名回填：DNS 优先，SNI 兜底，最后退化为裸 IP（含 IPv6 压缩写法）
    QString host = DnsSniffer::instance().resolve(ip);
    if (host.isEmpty())
        host = TlsSniParser::instance().resolve(ip);
    if (host.isEmpty())
        host = ip.toString();
    return host;
}

void TrafficStore::totals(quint64& upRate, quint64& downRate)
{
    QMutexLocker lock(&m_mutex);
    upRate = m_totalUpRate;
    downRate = m_totalDownRate;
}

void TrafficStore::peaks(quint64& upRate, quint64& downRate)
{
    QMutexLocker lock(&m_mutex);
    upRate = m_peakUpRate;
    downRate = m_peakDownRate;
}

void TrafficStore::reset()
{
    QMutexLocker lock(&m_mutex);
    m_byIp.clear();
    m_prevSnapshot.clear();
    m_lastSnapshotMs = 0;
    m_peakUpRate = 0;
    m_peakDownRate = 0;
    m_byPid.clear();
    m_prevPidSnapshot.clear();
    m_lastProcMs = 0;
    m_procCache.clear();
    m_domainCache.clear();
    m_siteCache.clear();
    m_siteMembers.clear();
    m_pidDetail.clear();
    m_samplePrevIp.clear();      // 重置后增量重新起算，避免虚高
    m_samplePrevIpSite.clear();
    m_samplePrevPid.clear();
    m_totalUpRate = 0;
    m_totalDownRate = 0;
}
