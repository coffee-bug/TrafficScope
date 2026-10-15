#include "PacketSource.h"
#include "DnsSniffer.h"
#include "EtwSniffer.h"
#include "NetInterfacePicker.h"
#include "PacketParser.h"
#include "ProcTraffic.h"
#include "TlsSniParser.h"
#include "TrafficStore.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <vector>

namespace {

IpKey sockAddrToKey(const sockaddr* sa)
{
    if (!sa)
        return IpKey();
    if (sa->sa_family == AF_INET) {
        auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
        return IpKey::fromV4(ntohl(sin->sin_addr.S_un.S_addr));
    }
    if (sa->sa_family == AF_INET6) {
        auto* sin6 = reinterpret_cast<const sockaddr_in6*>(sa);
        return IpKey::fromV6(sin6->sin6_addr.u.Byte);
    }
    return IpKey();
}

} // namespace

PacketSource& PacketSource::instance()
{
    static PacketSource s;
    return s;
}

PacketSource::PacketSource()
{
    // 默认路由会随插拔网线 / 切换 WiFi 变化，周期重评估并自动重挂
    m_recheck.setInterval(3000);
    m_recheck.setTimerType(Qt::CoarseTimer);
    connect(&m_recheck, &QTimer::timeout, this, &PacketSource::onRecheck);
    m_recheck.start();
}

PacketSource::~PacketSource()
{
    stopCapture();
}

QStringList PacketSource::availableDevices()
{
    return NetInterfacePicker::allDevices();
}

void PacketSource::updateLocalAddresses()
{
    m_localSet.clear();
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW || size == 0)
        return;

    std::vector<char> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size) != NO_ERROR)
        return;

    for (auto* a = addrs; a; a = a->Next) {
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr) {
                const auto family = u->Address.lpSockaddr->sa_family;
                if (family == AF_INET || family == AF_INET6) {
                    const IpKey k = sockAddrToKey(u->Address.lpSockaddr);
                    if (!k.isZero())
                        m_localSet.insert(k);
                }
            }
        }
    }
}

bool PacketSource::startCapture(QString& err, const QString& deviceName)
{
    if (isRunning())
        stopCapture();

    QString devName = deviceName;
    if (devName.isEmpty())
        devName = NetInterfacePicker::pickDevice();

    if (devName.isEmpty()) {
        err = deviceName.isEmpty()
                  ? QStringLiteral("未找到可用网卡（自动选择失败）")
                  : QStringLiteral("未找到指定网卡: %1").arg(deviceName);
        return false;
    }

    const uint32_t targetIfIndex = NetInterfacePicker::ifIndexForDevice(devName);
    updateLocalAddresses();

    auto packetHandler = [this](const uint8_t* bytes, uint32_t len, uint32_t ifIndex) {
        handlePacket(bytes, len, ifIndex);
    };

    if (!m_etwSource.start(targetIfIndex, std::move(packetHandler), err))
        return false;

    m_deviceName = devName;
    m_deviceDesc = NetInterfacePicker::deviceDescription(devName);
    m_currentIfIndex = targetIfIndex;

    return true;
}

void PacketSource::stopCapture()
{
    m_etwSource.stop();
}

bool PacketSource::restart(const QString& deviceName, QString& err)
{
    stopCapture();
    return startCapture(err, deviceName);
}

void PacketSource::setAutoSelect(bool on)
{
    if (m_auto == on)
        return;
    m_auto = on;
    if (on)
        onRecheck(); // 立即切回默认路由所在网卡
}

bool PacketSource::selectDevice(const QString& deviceName, QString& err)
{
    m_auto = deviceName.isEmpty(); // 传空串表示恢复自动
    const bool ok = restart(deviceName, err);
    if (ok && !m_auto)
        emit deviceChanged(m_deviceName);
    return ok;
}

void PacketSource::onRecheck()
{
    if (!m_auto)
        return;

    const QString dev = NetInterfacePicker::pickDevice();
    if (dev.isEmpty())
        return; // 无默认路由（如断网），保持现状等待恢复
    if (isRunning() && dev == m_deviceName)
        return;

    // 默认路由换了网卡，或抓包已中断：重新挂载
    QString err;
    if (startCapture(err))
        emit deviceChanged(m_deviceName);
}

void PacketSource::handlePacket(const uint8_t* bytes, uint32_t len, uint32_t /*ifIndex*/)
{
    ++m_pktTotal;

    ParsedPacket pp;
    // 约束3：以太网帧 m_linkOffset 必须为 0
    if (!PacketParser::parse(bytes, static_cast<int>(len), 0, pp))
        return;

    const bool srcLocal = m_localSet.contains(pp.srcIp);
    const bool dstLocal = m_localSet.contains(pp.dstIp);
    if (srcLocal == dstLocal) // 双方都不是本机（杂收流量）或本机内部流量
        return;

    ++m_pktCounted;

    const IpKey remote = srcLocal ? pp.dstIp : pp.srcIp;
    const bool upload = srcLocal;

    // 进程归因：本机侧 ip:port 反查连接表（PID）；短命连接可能查不到，归入"未归因流量"
    const IpKey localIp = srcLocal ? pp.srcIp : pp.dstIp;
    const uint16_t localPort = srcLocal ? pp.srcPort : pp.dstPort;
    const uint16_t remotePort = srcLocal ? pp.dstPort : pp.srcPort;
    const quint64 pid = ProcTraffic::instance().pidFor(
        pp.isTcp, localIp, localPort, remote, remotePort);
    if (pid == 0)
        EtwSniffer::instance().noteUnattributed(pp.isTcp, pp.isUdp, len);

    // 设置项：不再统计未归因流量 —— 三个维度（域名 / 总量 / 进程）都不计入。
    // 注意只丢字节，DNS / SNI 嗅探照常（下面那两段与 PID 无关，丢了会影响域名归因）。
    if (pid != 0 || TrafficStore::instance().countsUnattributed()) {
        TrafficStore::instance().addBytes(remote, upload, len);
        TrafficStore::instance().addProcBytes(pid, upload, len);
    }

    if (pp.isUdp && (pp.srcPort == 53 || pp.dstPort == 53)) {
        DnsSniffer::instance().feedResponse(pp.srcIp, pp.srcPort,
                                            pp.dstIp, pp.dstPort,
                                            pp.payload, pp.payloadLen);
    }
    if (pp.isTcp) {
        TlsSniParser::instance().feedTcp(pp.srcIp, pp.srcPort,
                                         pp.dstIp, pp.dstPort,
                                         pp.payload, pp.payloadLen);
    }
}
