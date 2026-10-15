#pragma once
#include "IpKey.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <QtGlobal>
#include <cstdint>

// 旁路嗅探 UDP 53 端口的 DNS 应答，维护 IP -> 域名 映射（带 TTL 自动过期）。
// 同时处理 A（IPv4）与 AAAA（IPv6）记录。
//
// 除了域名，这里还是「网站」维度归属的**数据源**：
// DNS 应答里的 CNAME 链是网站自己给出的声明 —— 链上每一跳（CDN 域名）都属于
// 链头那个网站。以前这条链只用来把 IP 回溯到链头、用完即弃，现在把它留下来
// （m_hostSite），于是走 CDN 的流量能自动归到主站，不需要内置任何站点知识。
class DnsSniffer {
public:
    static DnsSniffer& instance();

    // dns/len: 完整 DNS 报文（UDP 载荷）。仅处理应答方向（serverPort == 53）。
    void feedResponse(const IpKey& serverIp, uint16_t serverPort,
                      const IpKey& clientIp, uint16_t clientPort,
                      const uint8_t* dns, int len);

    // 未命中或已过期返回空串
    QString resolve(const IpKey& ip);

    // ---- 网站归属（供 core/SiteResolver 调用）----
    // shared 返回"这个 IP / 域名被多个网站解析过"（CDN 共用），归属存疑。
    QString siteOfIp(const IpKey& ip, bool* shared = nullptr);
    QString siteOfDomain(const QString& domain, bool* shared = nullptr);

private:
    DnsSniffer() = default;

    struct Entry {
        QString domain;
        QString site;        // 该 IP 所属网站（链头域名的注册域名）
        bool shared = false; // 曾被多个网站解析过
        qint64 expireAtMs = 0;
    };
    struct HostEntry {
        QString site;
        bool shared = false;
        qint64 expireAtMs = 0;
    };

    void addRecord(const IpKey& ip, const QString& name, const QString& site, quint32 ttl);
    void addHostSite(const QString& domain, const QString& site, quint32 ttl);
    void pruneLocked(qint64 nowMs);

    QMutex m_mutex;
    QHash<IpKey, Entry> m_map;             // 远端 IP -> 域名 + 网站
    QHash<QString, HostEntry> m_hostSite;  // 链上任意域名 -> 网站（含链头自己）
    int m_addCount = 0;
};
