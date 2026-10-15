#include "DnsSniffer.h"

#include "SiteName.h"

#include <QDateTime>
#include <QStringList>
#include <QVector>

namespace {

// DNS 报文头固定 12 字节
constexpr int kDnsHeaderLen = 12;
// TTL 下限：CDN 常返回很短的 TTL，按 30s 记，避免查表频繁未命中导致域名显示断档
constexpr quint32 kTtlMinSec = 30;
// 表容量兜底与清理频率：超容量整表清空；每插入这么多条清理一次过期项
constexpr int kMaxEntries = 100000;
constexpr quint32 kPruneEvery = 512;

inline uint16_t rd16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

inline uint32_t rd32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

// 从 msg 的 offset 处读取域名（处理压缩指针）。
// 返回读取结束后的下一个 offset（若发生跳转，返回跳转前 pos+2），失败返回 -1。
// 压缩指针靠 guard 计数防环，不做递归。
int readName(const uint8_t* msg, int len, int offset, QString& out)
{
    if (offset < 0 || offset >= len)
        return -1;
    int pos = offset;
    int nextPos = -1;
    int guard = 0;
    while (guard++ < 512) {
        if (pos >= len)
            return -1;
        const uint8_t l = msg[pos];
        if ((l & 0xC0) == 0xC0) {            // 压缩指针
            if (pos + 1 >= len)
                return -1;
            const int ptr = ((l & 0x3F) << 8) | msg[pos + 1];
            if (nextPos < 0)
                nextPos = pos + 2;
            pos = ptr;
            continue;
        }
        if (l == 0) {                        // 域名结束
            if (nextPos < 0)
                nextPos = pos + 1;
            return nextPos;
        }
        if (pos + 1 + l > len)
            return -1;
        if (!out.isEmpty())
            out += QLatin1Char('.');
        out += QString::fromLatin1(reinterpret_cast<const char*>(msg + pos + 1), l);
        pos += 1 + l;
    }
    return -1;
}

} // namespace

DnsSniffer& DnsSniffer::instance()
{
    static DnsSniffer s;
    return s;
}

void DnsSniffer::feedResponse(const IpKey& /*serverIp*/, uint16_t serverPort,
                              const IpKey& /*clientIp*/, uint16_t /*clientPort*/,
                              const uint8_t* dns, int len)
{
    if (serverPort != 53 || len < kDnsHeaderLen)
        return;
    if ((dns[2] & 0x80) == 0)                // QR=0 是查询，只处理应答
        return;

    const uint16_t qdCount = rd16(dns + 4);
    const uint16_t anCount = rd16(dns + 6);

    // 跳过 Question 区
    int pos = kDnsHeaderLen;
    QString skip;
    for (int i = 0; i < qdCount; ++i) {
        pos = readName(dns, len, pos, skip);
        if (pos < 0 || pos + 4 > len)
            return;
        pos += 4;
    }

    // 第一遍：把 Answer 区的 RR 全部读出来，顺手建好 CNAME 链。
    // 分两遍是必须的 —— A 记录有可能排在它的 CNAME 之前，一遍扫会漏掉链，
    // 那样 CDN 域名就归不到主站了（网站维度的核心依据）。
    struct RR {
        QString owner;
        uint16_t type = 0;
        quint32 ttl = 0;
        int rdata = 0;
        uint16_t rdlen = 0;
    };
    QVector<RR> rrs;
    rrs.reserve(anCount);
    QHash<QString, QString> cnameChain;      // CNAME 目标 -> 原始名
    for (int i = 0; i < anCount; ++i) {
        QString owner;
        pos = readName(dns, len, pos, owner);
        if (pos < 0 || pos + 10 > len)
            return;
        RR rr;
        rr.owner = owner;
        rr.type = rd16(dns + pos);
        rr.ttl = rd32(dns + pos + 4);
        rr.rdlen = rd16(dns + pos + 8);
        rr.rdata = pos + 10;
        pos += 10;
        if (pos + rr.rdlen > len)
            return;

        if (rr.type == 5 && rr.rdlen > 0) {  // CNAME
            QString target;
            if (readName(dns, len, rr.rdata, target) >= 0 && !target.isEmpty())
                cnameChain[target] = rr.owner;
        }
        rrs.append(rr);
        pos += rr.rdlen;
    }

    // 第二遍：只有 A / AAAA 才落库
    for (const RR& rr : rrs) {
        if (rr.type != 1 && rr.type != 28)
            continue;
        if (rr.type == 1 && rr.rdlen != 4)
            continue;
        if (rr.type == 28 && rr.rdlen != 16)
            continue;

        // 顺着链回溯到原始查询名，路上每一跳都算这个网站的资源
        QStringList path;
        QString origin = rr.owner;
        path.append(origin);
        for (int d = 0; d < 8 && cnameChain.contains(origin); ++d) {
            origin = cnameChain.value(origin);
            path.append(origin);
        }
        if (origin.isEmpty())
            continue;

        IpKey ip;
        if (rr.type == 1) {
            const quint32 v4 = rd32(dns + rr.rdata);
            if (v4 == 0)
                continue;
            ip = IpKey::fromV4(v4);
        } else {
            ip = IpKey::fromV6(dns + rr.rdata);
        }

        const QString site = ts::site::siteOfHost(origin);
        addRecord(ip, origin, site, rr.ttl);
        for (const QString& hop : path)
            addHostSite(hop, site, rr.ttl);
    }
}

QString DnsSniffer::resolve(const IpKey& ip)
{
    QMutexLocker lock(&m_mutex);
    auto it = m_map.find(ip);
    if (it == m_map.end())
        return QString();
    if (it->expireAtMs < QDateTime::currentMSecsSinceEpoch()) {
        m_map.erase(it);
        return QString();
    }
    return it->domain;
}

QString DnsSniffer::siteOfIp(const IpKey& ip, bool* shared)
{
    if (shared)
        *shared = false;
    QMutexLocker lock(&m_mutex);
    auto it = m_map.find(ip);
    if (it == m_map.end())
        return QString();
    if (it->expireAtMs < QDateTime::currentMSecsSinceEpoch()) {
        m_map.erase(it);
        return QString();
    }
    if (shared)
        *shared = it->shared;
    return it->site;
}

QString DnsSniffer::siteOfDomain(const QString& domain, bool* shared)
{
    if (shared)
        *shared = false;
    if (domain.isEmpty())
        return QString();
    QMutexLocker lock(&m_mutex);
    auto it = m_hostSite.find(domain);
    if (it == m_hostSite.end())
        return QString();
    if (it->expireAtMs < QDateTime::currentMSecsSinceEpoch()) {
        m_hostSite.erase(it);
        return QString();
    }
    if (shared)
        *shared = it->shared;
    return it->site;
}

void DnsSniffer::addRecord(const IpKey& ip, const QString& name, const QString& site,
                           quint32 ttl)
{
    QMutexLocker lock(&m_mutex);
    const qint64 ttlMs = qint64(ttl < kTtlMinSec ? kTtlMinSec : ttl) * 1000;
    const qint64 expireAtMs = QDateTime::currentMSecsSinceEpoch() + ttlMs;

    auto it = m_map.find(ip);
    if (it == m_map.end()) {
        Entry e;
        e.domain = name;
        e.site = site;
        e.expireAtMs = expireAtMs;
        m_map.insert(ip, e);
    } else {
        // 同一个 IP 被另一个网站解析过 → 标记"共享"（CDN 复用），
        // 归属取最近一次，由 UI 如实标出来，不静默乱并。
        if (!it->site.isEmpty() && it->site != site)
            it->shared = true;
        it->domain = name;
        it->site = site;
        it->expireAtMs = expireAtMs;
    }

    if (m_map.size() > kMaxEntries)          // 内存兜底
        m_map.clear();
    if (++m_addCount % kPruneEvery == 0)
        pruneLocked(QDateTime::currentMSecsSinceEpoch());
}

void DnsSniffer::addHostSite(const QString& domain, const QString& site, quint32 ttl)
{
    if (domain.isEmpty() || site.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    const qint64 ttlMs = qint64(ttl < kTtlMinSec ? kTtlMinSec : ttl) * 1000;
    const qint64 expireAtMs = QDateTime::currentMSecsSinceEpoch() + ttlMs;

    auto it = m_hostSite.find(domain);
    if (it == m_hostSite.end()) {
        HostEntry e;
        e.site = site;
        e.expireAtMs = expireAtMs;
        m_hostSite.insert(domain, e);
    } else {
        if (it->site != site)
            it->shared = true;
        it->site = site;
        it->expireAtMs = expireAtMs;
    }
    if (m_hostSite.size() > 100000)
        m_hostSite.clear();
}

void DnsSniffer::pruneLocked(qint64 nowMs)
{
    for (auto it = m_map.begin(); it != m_map.end();) {
        if (it->expireAtMs < nowMs)
            it = m_map.erase(it);
        else
            ++it;
    }
    for (auto it = m_hostSite.begin(); it != m_hostSite.end();) {
        if (it->expireAtMs < nowMs)
            it = m_hostSite.erase(it);
        else
            ++it;
    }
}
