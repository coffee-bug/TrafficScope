#pragma once
#include "IpKey.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <QtGlobal>
#include <cstdint>

// 从 TCP 443（及任意端口的 TLS 握手）的 ClientHello 中提取 SNI，
// 作为 DNS 嗅探缺失时的兜底（例如 DoH、缓存未命中的直连）。
class TlsSniParser {
public:
    static TlsSniParser& instance();

    // payload/len: TCP 载荷。命中 ClientHello 时记录 serverIp -> 域名。
    void feedTcp(const IpKey& srcIp, uint16_t srcPort,
                 const IpKey& dstIp, uint16_t dstPort,
                 const uint8_t* payload, int len);

    // 未命中返回空串
    QString resolve(const IpKey& ip);

private:
    TlsSniParser() = default;

    static QString extractSni(const uint8_t* p, int len);

    QMutex m_mutex;
    QHash<IpKey, QString> m_ipToDomain;
};
