#include "PacketParser.h"

namespace {

inline uint16_t rd16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

inline uint32_t rd32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

// 传输层清零：无端口信息时（ICMP、非首片、扩展头链异常）调用，只计字节不做端口归因
inline void clearTransport(ParsedPacket& out)
{
    out.isTcp = out.isUdp = false;
    out.payload = nullptr;
    out.payloadLen = 0;
    out.srcPort = out.dstPort = 0;
}

} // namespace

bool PacketParser::parse(const uint8_t* data, int len, int linkOffset, ParsedPacket& out)
{
    if (len < linkOffset + 14)
        return false;

    uint16_t ethType = rd16(data + linkOffset + 12);
    int ipOff = linkOffset + 14;
    if (ethType == 0x8100) {                 // 802.1Q VLAN
        if (len < linkOffset + 18)
            return false;
        ethType = rd16(data + linkOffset + 16);
        ipOff = linkOffset + 18;
    }

    if (ethType == 0x0800)
        return parseIpv4(data + ipOff, len - ipOff, out);
    if (ethType == 0x86DD)
        return parseIpv6(data + ipOff, len - ipOff, out);
    return false;                            // ARP / 其他链路层协议不计入
}

bool PacketParser::parseIpv4(const uint8_t* ip, int avail, ParsedPacket& out)
{
    if (avail < 20 || (ip[0] >> 4) != 4)
        return false;

    const int ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || avail < ihl)
        return false;

    out.srcIp = IpKey::fromV4(rd32(ip + 12));
    out.dstIp = IpKey::fromV4(rd32(ip + 16));

    // 非首片（分片偏移非 0）无端口信息，直接计入字节但不做端口归因
    const uint16_t flagsFrag = rd16(ip + 6);
    if ((flagsFrag & 0x1FFF) != 0) {
        clearTransport(out);
        return true;
    }

    const uint8_t proto = ip[9];
    if (proto != 6 && proto != 17) {         // ICMP 等其他协议
        clearTransport(out);
        return true;
    }
    return parseTransport(ip + ihl, avail - ihl, proto, out);
}

bool PacketParser::parseIpv6(const uint8_t* ip, int avail, ParsedPacket& out)
{
    if (avail < 40 || (ip[0] >> 4) != 6)
        return false;

    out.srcIp = IpKey::fromV6(ip + 8);
    out.dstIp = IpKey::fromV6(ip + 24);

    // 逐跳遍历扩展头链，直到遇到 TCP/UDP 或无下一个头
    uint8_t next = ip[6];
    int pos = 40;
    for (int hop = 0; hop < 8; ++hop) {
        if (next == 6 || next == 17)
            return parseTransport(ip + pos, avail - pos, next, out);
        if (next == 59) {                    // No Next Header
            clearTransport(out);
            return true;
        }

        int hdrLen = 0;
        uint8_t follow = 0;
        if (next == 44) {                    // 分片头：固定 8 字节
            if (avail < pos + 8)
                return false;
            const uint16_t frag = rd16(ip + pos + 2);
            const uint16_t offset = uint16_t((frag >> 3) & 0x1FFF);
            if (offset != 0) {               // 非首片，无端口信息
                clearTransport(out);
                return true;
            }
            follow = ip[pos];
            hdrLen = 8;
        } else if (next == 51) {             // AH：长度字段单位是 4 字节且减 2
            if (avail < pos + 2)
                return false;
            follow = ip[pos];
            hdrLen = (ip[pos + 1] + 2) * 4;
        } else {                             // 0/43/60 等：长度字段单位 8 字节
            if (avail < pos + 2)
                return false;
            follow = ip[pos];
            hdrLen = (ip[pos + 1] + 1) * 8;
        }

        if (hdrLen <= 0 || pos + hdrLen > avail)
            return false;
        pos += hdrLen;
        next = follow;
    }

    clearTransport(out);                     // 扩展头链过长/异常：只计字节
    return true;
}

bool PacketParser::parseTransport(const uint8_t* tp, int avail, uint8_t proto,
                                  ParsedPacket& out)
{
    if (avail < 4)
        return false;
    out.srcPort = rd16(tp);
    out.dstPort = rd16(tp + 2);

    if (proto == 6) {                        // TCP
        out.isTcp = true;
        out.isUdp = false;
        if (avail < 20)
            return false;
        int doff = (tp[12] >> 4) * 4;
        // 上面已保证 avail >= 20；doff 非法时按最小 TCP 头 20 字节处理
        if (doff < 20 || doff > avail)
            doff = 20;
        out.payload = tp + doff;
        out.payloadLen = avail - doff;
    } else {                                 // UDP
        out.isTcp = false;
        out.isUdp = true;
        if (avail < 8)
            return false;
        out.payload = tp + 8;
        out.payloadLen = avail - 8;
    }
    return true;
}
