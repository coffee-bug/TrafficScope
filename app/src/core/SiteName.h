#pragma once
#include <QString>

namespace ts {
namespace site {

// 「网站」维度用的域名工具：纯字符串运算，不依赖抓包数据、不查网络。
//
// 网站 = 域名的**注册域名**（eTLD+1）：www.baidu.com → baidu.com。
// 这样同一站点散落在 www / m / api / img 等子域上的流量才会合到一行。
//
// 要保留几段取决于后缀是几段（com.cn 得留 3 段、github.io 得留 3 段），
// 所以内置一张精简多级后缀表；表外的按"最后两段"处理。
// 裸 IP 字面量没有域名可言，原样返回（它自己就是一行）。

// "1.2.3.4" / "2001:db8::1" → true（含冒号一律按 IPv6 处理）
bool isIpLiteral(const QString& host);

// 小写化 + 去掉 DNS 里末尾的根点（a.com. 与 a.com 是同一个域名）
QString normalizeHost(const QString& host);

// 注册域名（eTLD+1）。IP / 单段名（localhost）/ 空串原样返回。
QString registeredDomain(const QString& host);

// 网站名：域名 → 注册域名；IP → 原样
QString siteOfHost(const QString& host);

}  // namespace site
}  // namespace ts
