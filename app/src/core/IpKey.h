#pragma once
#include <QLatin1Char>
#include <QString>
#include <QtGlobal>
#include <cstdint>
#include <cstring>

// 统一的 IP 地址 key：始终以 16 字节存储，IPv4 用 IPv4-mapped IPv6 形式表示
// （::ffff:a.b.c.d，即前 10 字节为 0、第 11~12 字节为 0xFF 0xFF）。
//
// 这样 IPv4 / IPv6 在 TrafficStore、DnsSniffer、TlsSniParser、ProcTraffic
// 中共用同一套容器与逻辑，不必为两套协议各写一份。
struct IpKey {
    uint8_t b[16];

    IpKey() { std::memset(b, 0, sizeof(b)); }

    bool operator==(const IpKey& o) const { return std::memcmp(b, o.b, sizeof(b)) == 0; }

    bool isV4() const
    {
        return b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0
            && b[5] == 0 && b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0
            && b[10] == 0xFF && b[11] == 0xFF;
    }

    bool isZero() const
    {
        for (int i = 0; i < 16; ++i)
            if (b[i] != 0)
                return false;
        return true;
    }

    // hostOrder 为主机序的 32 位 IPv4（与旧代码一致）
    static IpKey fromV4(uint32_t hostOrder)
    {
        IpKey k;
        k.b[10] = 0xFF;
        k.b[11] = 0xFF;
        k.b[12] = uint8_t((hostOrder >> 24) & 0xFF);
        k.b[13] = uint8_t((hostOrder >> 16) & 0xFF);
        k.b[14] = uint8_t((hostOrder >> 8) & 0xFF);
        k.b[15] = uint8_t(hostOrder & 0xFF);
        return k;
    }

    // p 指向 16 字节的 IPv6 地址（网络字节序，即线序）
    static IpKey fromV6(const uint8_t* p)
    {
        IpKey k;
        std::memcpy(k.b, p, 16);
        return k;
    }

    // "192.0.2.1" 或 "2001:db8::1"（RFC 5952 压缩写法）
    QString toString() const
    {
        if (isV4())
            return QStringLiteral("%1.%2.%3.%4").arg(b[12]).arg(b[13]).arg(b[14]).arg(b[15]);

        uint16_t w[8];
        for (int i = 0; i < 8; ++i)
            w[i] = uint16_t((uint16_t(b[i * 2]) << 8) | b[i * 2 + 1]);

        // 找最长的一段连续 0（>=2 段才压缩）
        int bestStart = -1;
        int bestLen = 0;
        int curStart = -1;
        int curLen = 0;
        for (int i = 0; i < 8; ++i) {
            if (w[i] == 0) {
                if (curStart < 0) {
                    curStart = i;
                    curLen = 1;
                } else {
                    ++curLen;
                }
                if (curLen > bestLen) {
                    bestLen = curLen;
                    bestStart = curStart;
                }
            } else {
                curStart = -1;
                curLen = 0;
            }
        }
        if (bestLen < 2) {
            bestStart = -1;
            bestLen = 0;
        }

        QString out;
        bool needSep = false;
        for (int i = 0; i < 8; ++i) {
            if (i == bestStart) {
                out += QStringLiteral("::");
                i += bestLen - 1;
                needSep = false;
                continue;
            }
            if (needSep)
                out += QLatin1Char(':');
            out += QString::number(uint(w[i]), 16);
            needSep = true;
        }
        return out;
    }
};

inline uint qHash(const IpKey& k, uint seed = 0)
{
    uint h = seed ? seed : 2166136261u;   // FNV-1a
    for (int i = 0; i < 16; ++i) {
        h ^= uint(k.b[i]);
        h *= 16777619u;
    }
    return h;
}

// 连接 key：本机与远端的 16 字节地址 + 端口（IPv4 以 mapped 形式存放，
// 因此 IPv4 / IPv6 共用同一张表）。UDP 无远端，remote 恒 0。
// ProcTraffic（轮询快照）与 EtwSniffer（ETW 实时映射）共用同一套 key。
struct TsConnKey {
    IpKey localAddr;
    uint16_t localPort = 0;
    IpKey remoteAddr;
    uint16_t remotePort = 0;
    bool operator==(const TsConnKey& o) const
    {
        return localAddr == o.localAddr && localPort == o.localPort
            && remoteAddr == o.remoteAddr && remotePort == o.remotePort;
    }
};

inline uint qHash(const TsConnKey& k, uint seed = 0)
{
    uint h = seed ? seed : 2166136261u;
    for (int i = 0; i < 16; ++i) {
        h ^= uint(k.localAddr.b[i]);
        h *= 16777619u;
    }
    h ^= uint(k.localPort);
    h *= 16777619u;
    for (int i = 0; i < 16; ++i) {
        h ^= uint(k.remoteAddr.b[i]);
        h *= 16777619u;
    }
    h ^= uint(k.remotePort);
    h *= 16777619u;
    return h;
}
