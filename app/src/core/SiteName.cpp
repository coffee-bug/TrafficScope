#include "SiteName.h"

#include <QLatin1Char>
#include <QSet>
#include <QStringList>

namespace ts {
namespace site {

namespace {

// 多级后缀：命中时保留「后缀段数 + 1」段，按最长匹配优先。
//
// 这份表是**精简版**（不是完整 Public Suffix List）：只收常用国别二级后缀与
// "每个子域都是独立网站"的托管域（github.io 这类），表外一律按最后两段处理。
//
// 刻意**不收 CDN 域名**（cloudfront.net / alicdn.com 等）：它们的主站归属由
// DNS CNAME 链解决（见 SiteResolver），写进表只会把归属搞得更乱。
bool isMultiLabelSuffix(const QString& suffix)
{
    static const QSet<QString> table = [] {
        QSet<QString> t;
        const char* const raw[] = {
            // 中国（含省份二级域）
            "com.cn", "net.cn", "org.cn", "gov.cn", "edu.cn", "ac.cn", "mil.cn",
            "ah.cn", "bj.cn", "cq.cn", "fj.cn", "gd.cn", "gs.cn", "gz.cn", "gx.cn",
            "ha.cn", "hb.cn", "he.cn", "hi.cn", "hl.cn", "hn.cn", "jl.cn", "js.cn",
            "jx.cn", "ln.cn", "nm.cn", "nx.cn", "qh.cn", "sc.cn", "sd.cn", "sh.cn",
            "sn.cn", "sx.cn", "tj.cn", "xj.cn", "xz.cn", "yn.cn", "zj.cn",
            // 港澳台
            "com.hk", "net.hk", "org.hk", "edu.hk", "gov.hk", "idv.hk",
            "com.mo", "net.mo", "org.mo", "edu.mo", "gov.mo",
            "com.tw", "net.tw", "org.tw", "edu.tw", "gov.tw", "idv.tw",
            // 英国 / 日本 / 韩国
            "co.uk", "org.uk", "me.uk", "net.uk", "ac.uk", "gov.uk", "sch.uk",
            "police.uk", "nhs.uk", "ltd.uk", "plc.uk",
            "co.jp", "or.jp", "ne.jp", "ac.jp", "go.jp", "ed.jp", "gr.jp", "lg.jp",
            "co.kr", "or.kr", "ne.kr", "go.kr", "ac.kr", "ms.kr", "es.kr", "sc.kr",
            // 大洋洲
            "com.au", "net.au", "org.au", "edu.au", "gov.au", "id.au", "asn.au",
            "co.nz", "net.nz", "org.nz", "govt.nz", "ac.nz", "geek.nz", "maori.nz",
            // 亚洲其他
            "co.in", "net.in", "org.in", "gen.in", "firm.in", "ind.in", "nic.in",
            "co.id", "or.id", "web.id", "sch.id", "go.id",
            "co.th", "or.th", "net.th", "go.th", "ac.th",
            "com.sg", "net.sg", "org.sg", "edu.sg", "gov.sg",
            "com.my", "net.my", "org.my", "edu.my", "gov.my",
            "com.ph", "net.ph", "org.ph", "edu.ph", "gov.ph",
            "com.vn", "net.vn", "org.vn", "edu.vn", "gov.vn",
            "com.pk", "net.pk", "org.pk", "edu.pk", "gov.pk",
            "com.bd", "net.bd", "org.bd", "edu.bd", "gov.bd",
            "com.lk", "co.lk", "com.np", "com.mm", "com.kh", "com.la",
            // 中东 / 非洲
            "com.sa", "net.sa", "org.sa", "edu.sa", "gov.sa",
            "com.eg", "com.ng", "co.ke", "co.tz", "co.ug", "co.zw", "co.za",
            "com.tr", "co.il", "org.il", "net.il", "ac.il", "gov.il",
            // 欧洲 / 独联体
            "com.es", "com.pt", "com.gr", "com.pl", "com.ua", "com.ru", "com.by",
            "com.kz", "com.ge", "com.am", "com.az", "com.md", "com.rs", "com.hr",
            "com.cy", "com.al", "com.mk", "com.ba", "com.ee", "com.lv", "com.lt",
            // 美洲
            "com.mx", "com.br", "net.br", "org.br", "gov.br", "edu.br",
            "com.ar", "com.co", "com.pe", "com.ve", "com.ec", "com.uy", "com.py",
            "com.bo", "com.cl", "com.do", "com.gt", "com.pa", "com.ni", "com.jm",
            // 托管 / 私有域：每个子域都是独立网站，必须多留一段
            "github.io", "gitlab.io", "codeberg.page", "sourceforge.io",
            "pages.dev", "web.app", "netlify.app", "vercel.app", "workers.dev",
            "r2.dev", "firebaseapp.com", "appspot.com", "cloudfunctions.net",
            "azurewebsites.net", "herokuapp.com", "glitch.me", "onedev.io",
        };
        for (const char* p : raw)
            t.insert(QString::fromLatin1(p));
        return t;
    }();
    return table.contains(suffix);
}

}  // namespace

bool isIpLiteral(const QString& host)
{
    if (host.isEmpty())
        return false;
    if (host.contains(QLatin1Char(':')))
        return true;                      // IPv6（含 ::ffff:1.2.3.4 这种映射写法）
    const QStringList p = host.split(QLatin1Char('.'));
    if (p.size() != 4)
        return false;
    for (const QString& seg : p) {
        if (seg.isEmpty() || seg.size() > 3)
            return false;
        for (const QChar c : seg) {
            if (!c.isDigit())
                return false;
        }
    }
    return true;
}

QString normalizeHost(const QString& host)
{
    QString h = host.trimmed().toLower();
    while (h.endsWith(QLatin1Char('.')))
        h.chop(1);
    return h;
}

QString registeredDomain(const QString& host)
{
    const QString h = normalizeHost(host);
    if (h.isEmpty() || isIpLiteral(h))
        return h;
    const QStringList parts = h.split(QLatin1Char('.'));
    if (parts.size() < 2)
        return h;                          // localhost 这类单段名

    // 最长匹配：后缀最多按 3 段试（表内目前只有 2 段，留一格给将来）
    for (int n = 3; n >= 2; --n) {
        if (parts.size() <= n)
            continue;
        const QString suffix = QStringList(parts.mid(parts.size() - n)).join(QLatin1Char('.'));
        if (isMultiLabelSuffix(suffix))
            return QStringList(parts.mid(parts.size() - n - 1)).join(QLatin1Char('.'));
    }
    return QStringList(parts.mid(parts.size() - 2)).join(QLatin1Char('.'));
}

QString siteOfHost(const QString& host)
{
    const QString h = normalizeHost(host);
    if (h.isEmpty() || isIpLiteral(h))
        return h;                          // 裸 IP 自己就是一行
    return registeredDomain(h);
}

}  // namespace site
}  // namespace ts
