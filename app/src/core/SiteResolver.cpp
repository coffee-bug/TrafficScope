#include "SiteResolver.h"

#include "DnsSniffer.h"
#include "SiteName.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace ts {

namespace {

// 扩展给的归属多久失效：它只在页面活动期间回传，长时间没刷新就当作过期。
// 过期后 browserSiteFor 返回空，本次归属不算 Browser —— 而「网站」维度只认 Browser，
// 于是这段流量不进网站维度（宁可少一点，也不要拿几天前的页面信息给今天的流量定性）。
const qint64 kBrowserTtlMs = 30 * 60 * 1000;
// 内存兜底：极端情况下（爬了大量站点）别让表无限长
const int kMaxBrowserEntries = 50000;

}  // namespace

SiteResolver& SiteResolver::instance()
{
    static SiteResolver s;
    return s;
}

SiteInfo SiteResolver::resolve(const IpKey& ip, const QString& host)
{
    const QString h = site::normalizeHost(host);

    // ① 浏览器扩展：最准的一条线索（它知道是哪个页面发起的请求）
    if (!h.isEmpty()) {
        const QString s = browserSiteFor(h, ip);
        if (!s.isEmpty())
            return {s, false, SiteSource::Browser};
    }

    bool shared = false;
    if (!h.isEmpty() && !site::isIpLiteral(h)) {
        // ② 这个域名曾在某条 CNAME 链上出现过 → 归到链头那个网站
        const QString s = DnsSniffer::instance().siteOfDomain(h, &shared);
        if (!s.isEmpty())
            return {s, shared, SiteSource::Cname};
        // ③ 没线索 → 它自己的注册域名（www.x.com → x.com）
        const QString own = site::siteOfHost(h);
        return {own, false, own.isEmpty() ? SiteSource::None : SiteSource::RegDomain};
    }

    // 裸 IP（QUIC 无 SNI / DoH / 直连 IP）
    const QString s = DnsSniffer::instance().siteOfIp(ip, &shared);
    if (!s.isEmpty())
        return {s, shared, SiteSource::Cname};
    return {};
}

QString SiteResolver::browserSiteFor(const QString& host, const IpKey& ip)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker lock(&m_browserMutex);

    auto it = m_browserDomain.find(host);
    if (it != m_browserDomain.end()) {
        if (it->expireAtMs >= now)
            return it->site;
        m_browserDomain.erase(it);
    }
    if (!ip.isZero()) {
        auto iit = m_browserIp.find(ip);
        if (iit != m_browserIp.end()) {
            if (iit->expireAtMs >= now)
                return iit->site;
            m_browserIp.erase(iit);
        }
    }
    return QString();
}

void SiteResolver::feedBrowserMappings(const QVector<BrowserMapping>& items)
{
    if (items.isEmpty())
        return;

    const qint64 expireAt = QDateTime::currentMSecsSinceEpoch() + kBrowserTtlMs;
    QMutexLocker lock(&m_browserMutex);
    if (m_browserDomain.size() > kMaxBrowserEntries)
        m_browserDomain.clear();
    if (m_browserIp.size() > kMaxBrowserEntries)
        m_browserIp.clear();

    for (const BrowserMapping& m : items) {
        // 站点名统一按注册域名归一（扩展给的是 initiator 的 hostname，
        // 可能是 www.x.com，得跟 DNS 那条路出来的口径一致）
        const QString site = site::siteOfHost(m.site);
        if (site.isEmpty())
            continue;

        const QString domain = site::normalizeHost(m.domain);
        if (!domain.isEmpty()) {
            BrowserEntry e;
            e.site = site;
            e.expireAtMs = expireAt;
            m_browserDomain.insert(domain, e);
        }
        if (!m.ip.isEmpty()) {
            // 只接受 IPv4 字面量（扩展给的 ip 就是点分十进制）；
            // IPv6 走域名那条路，不影响正确性
            bool ok4 = false;
            const QStringList parts = m.ip.split(QLatin1Char('.'));
            if (parts.size() == 4) {
                quint32 v = 0;
                ok4 = true;
                for (const QString& p : parts) {
                    bool nok = false;
                    const uint n = p.toUInt(&nok);
                    if (!nok || n > 255) {
                        ok4 = false;
                        break;
                    }
                    v = (v << 8) | n;
                }
                if (ok4) {
                    BrowserEntry e;
                    e.site = site;
                    e.expireAtMs = expireAt;
                    m_browserIp.insert(IpKey::fromV4(v), e);
                }
            }
        }
    }
}

int SiteResolver::feedBrowserJson(const QByteArray& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return 0;
    const QJsonObject obj = doc.object();
    // 只认自己约定的消息类型，避免别的程序往这个管道里塞东西
    if (obj.value(QLatin1String("type")).toString() != QLatin1String("site"))
        return 0;

    const QJsonArray arr = obj.value(QLatin1String("items")).toArray();
    QVector<BrowserMapping> items;
    items.reserve(int(arr.size()));
    for (const QJsonValue& v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        BrowserMapping m;
        m.site = o.value(QLatin1String("site")).toString();
        m.domain = o.value(QLatin1String("domain")).toString();
        m.ip = o.value(QLatin1String("ip")).toString();
        if (!m.site.isEmpty())
            items.append(m);
    }
    if (items.isEmpty())
        return 0;
    feedBrowserMappings(items);
    return int(items.size());
}

}  // namespace ts
