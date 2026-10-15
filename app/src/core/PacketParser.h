#pragma once
#include "IpKey.h"

#include <QtGlobal>
#include <cstdint>

struct ParsedPacket {
    IpKey srcIp;
    IpKey dstIp;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    bool isTcp = false;
    bool isUdp = false;
    const uint8_t* payload = nullptr;   // TCP/UDP 载荷起点
    int payloadLen = 0;
};

// 以太网/VLAN/IPv4/IPv6/TCP/UDP 解析器（纯函数，无状态）。
// IPv4 以 IPv4-mapped IPv6 形式放入 IpKey，上层无需区分协议族。
class PacketParser {
public:
    // data: 从链路层头开始；linkOffset 由数据链路类型决定
    //（以太网为 0，PacketParser 自行解析以太网头）
    static bool parse(const uint8_t* data, int len, int linkOffset, ParsedPacket& out);

private:
    static bool parseIpv4(const uint8_t* ip, int avail, ParsedPacket& out);
    static bool parseIpv6(const uint8_t* ip, int avail, ParsedPacket& out);
    static bool parseTransport(const uint8_t* tp, int avail, uint8_t proto,
                               ParsedPacket& out);
};
