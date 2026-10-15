#pragma once
#include "IpKey.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

namespace ts {

// 归属来源
enum class SiteSource {
    None,       // 归不到
    Browser,    // 浏览器扩展回传
    Cname,      // DNS CNAME 链
    RegDomain,  // 域名自身的注册域名
};

// 归属结果
struct SiteInfo {
    QString site;          // 网站名（注册域名）；空 = 归不到（裸 IP 且无线索）
    bool shared = false;   // 这个域名/IP 被多个网站解析过（CDN 共用），归属存疑
    SiteSource source = SiteSource::None;
};

// 浏览器扩展回传的一条归属（只有域名级信息，没有 URL）
struct BrowserMapping {
    QString site;      // 发起页的 hostname（如 www.baidu.com）
    QString domain;    // 资源的 hostname（如 img.bdstatic.com）
    QString ip;        // 资源的远端 IP，可能为空
};

// 「网站」归属的**唯一出口**。
//
// 优先级：浏览器扩展 > DNS CNAME 链 > 域名自身的注册域名，结果带 source，
// 调用方按 source 决定收不收 —— 「网站」维度只认 Browser（见 TrafficStore）。
class SiteResolver {
public:
    static SiteResolver& instance();

    // 给一个远端定网站。ip 是抓包看到的远端地址，host 是该 IP 已回填出的显示名
    // （DNS 域名 / SNI 域名 / 裸 IP 字符串，由调用方决定）。
    SiteInfo resolve(const IpKey& ip, const QString& host);

    // ---- 浏览器扩展数据源（二期）----
    void feedBrowserMappings(const QVector<BrowserMapping>& items);
    // 直接吃 host 进程转发来的原始 JSON（{"type":"site","items":[...]}）。
    // 返回这一批吃到的**条数**（空帧/坏帧返回 0）—— 调用方据此更新
    // "最近一次收到数据"的时间与条数，否则空帧也会把状态刷成"刚刚收到"。
    int feedBrowserJson(const QByteArray& json);

private:
    SiteResolver() = default;

    QString browserSiteFor(const QString& host, const IpKey& ip);

    struct BrowserEntry {
        QString site;
        qint64 expireAtMs = 0;
    };

    QMutex m_browserMutex;
    QHash<QString, BrowserEntry> m_browserDomain;   // 资源域名 -> 网站
    QHash<IpKey, BrowserEntry> m_browserIp;         // 资源 IP -> 网站
};

}  // namespace ts
