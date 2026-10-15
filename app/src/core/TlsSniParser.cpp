#include "TlsSniParser.h"

namespace {

inline uint16_t rd16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

} // namespace

TlsSniParser& TlsSniParser::instance()
{
    static TlsSniParser s;
    return s;
}

void TlsSniParser::feedTcp(const IpKey& srcIp, uint16_t srcPort,
                           const IpKey& dstIp, uint16_t dstPort,
                           const uint8_t* payload, int len)
{
    if (len < 6 || payload[0] != 0x16)       // 仅 TLS Handshake 记录
        return;
    if (payload[1] != 0x03)                  // 版本高字节 0x03（SSLv3/TLS1.x）
        return;

    const QString sni = extractSni(payload, len);
    if (sni.isEmpty())
        return;

    IpKey serverIp = dstIp;
    if (srcPort == 443 && dstPort != 443)
        serverIp = srcIp;

    QMutexLocker lock(&m_mutex);
    // 表容量兜底：超了整表清空，防长期运行内存膨胀
    constexpr int kMaxEntries = 100000;
    if (m_ipToDomain.size() > kMaxEntries)
        m_ipToDomain.clear();
    m_ipToDomain.insert(serverIp, sni);
}

QString TlsSniParser::resolve(const IpKey& ip)
{
    QMutexLocker lock(&m_mutex);
    return m_ipToDomain.value(ip);
}

QString TlsSniParser::extractSni(const uint8_t* p, int len)
{
    // TLS Record Header: ContentType(1) Version(2) Length(2)
    if (len < 5)
        return QString();
    int pos = 5;                             // Handshake 起点

    // Handshake: Type(1) Length(3)
    if (pos + 4 > len || p[pos] != 0x01)     // 仅 ClientHello
        return QString();
    pos += 4;

    // ClientHello: Version(2) Random(32)
    if (pos + 34 > len)
        return QString();
    pos += 34;

    // Session ID: len(1) + data
    if (pos + 1 > len)
        return QString();
    pos += 1 + p[pos];

    // Cipher Suites: len(2) + data
    if (pos + 2 > len)
        return QString();
    pos += 2 + rd16(p + pos);

    // Compression Methods: len(1) + data
    if (pos + 1 > len)
        return QString();
    pos += 1 + p[pos];

    // Extensions: len(2) + data
    if (pos + 2 > len)
        return QString();
    const int extTotal = rd16(p + pos);
    pos += 2;
    const int extEnd = (pos + extTotal <= len) ? pos + extTotal : len;

    while (pos + 4 <= extEnd) {
        const uint16_t type = rd16(p + pos);
        const uint16_t elen = rd16(p + pos + 2);
        pos += 4;
        if (pos + elen > extEnd)
            return QString();

        if (type == 0x0000) {                // server_name 扩展
            int d = pos;
            if (d + 2 > pos + elen)
                return QString();
            const int listLen = rd16(p + d);
            d += 2;
            const int listEnd = (d + listLen <= pos + elen) ? d + listLen : pos + elen;
            while (d + 3 <= listEnd) {
                const uint8_t nameType = p[d];
                const uint16_t nlen = rd16(p + d + 1);
                d += 3;
                if (nameType == 0 && d + nlen <= listEnd && nlen > 0)
                    return QString::fromLatin1(reinterpret_cast<const char*>(p + d), nlen);
                d += nlen;
            }
            return QString();
        }
        pos += elen;
    }
    return QString();
}
