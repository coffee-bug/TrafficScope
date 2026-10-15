#include "NetInterfacePicker.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   // GetBestRoute2 / GetIpForwardTable2 需要 Vista+
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
#include <netioapi.h>

#include <vector>

namespace {

uint32_t sockAddrToHostOrder(const sockaddr* sa)
{
    auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
    return ntohl(sin->sin_addr.S_un.S_addr);
}

// 统一获取系统中所有适配器信息
std::vector<char> fetchAdapters()
{
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW || size == 0)
        return {};

    std::vector<char> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size) != NO_ERROR)
        return {};
    return buf;
}

} // namespace

bool NetInterfacePicker::bestRouteToInternet(uint32_t& ifIndex)
{
    constexpr const char* kProbeIp = "8.8.8.8";
    SOCKADDR_INET dst;
    memset(&dst, 0, sizeof(dst));
    dst.si_family = AF_INET;
    dst.Ipv4.sin_addr.S_un.S_addr = inet_addr(kProbeIp);

    SOCKADDR_INET src;
    memset(&src, 0, sizeof(src));

    MIB_IPFORWARD_ROW2 row;
    memset(&row, 0, sizeof(row));

    if (GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &row, &src) != NO_ERROR)
        return false;
    if (row.InterfaceIndex == 0)
        return false;
    ifIndex = row.InterfaceIndex;
    return true;
}

bool NetInterfacePicker::defaultRouteFromTable(uint32_t& ifIndex)
{
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR || !table)
        return false;

    uint32_t bestMetric = 0xFFFFFFFFu;
    bool found = false;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& r = table->Table[i];
        if (r.DestinationPrefix.Prefix.si_family != AF_INET)
            continue;
        if (r.DestinationPrefix.PrefixLength != 0)
            continue;
        if (r.NextHop.si_family != AF_INET)
            continue;
        if (found && r.Metric >= bestMetric)
            continue;
        bestMetric = r.Metric;
        ifIndex = r.InterfaceIndex;
        found = true;
    }
    FreeMibTable(table);
    return found;
}

uint32_t NetInterfacePicker::adapterIpv4ByIndex(uint32_t ifIndex)
{
    auto buf = fetchAdapters();
    if (buf.empty())
        return 0;

    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    for (auto* a = addrs; a; a = a->Next) {
        if (a->IfIndex != ifIndex || a->OperStatus != IfOperStatusUp)
            continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET)
                return sockAddrToHostOrder(u->Address.lpSockaddr);
        }
    }
    return 0;
}

QStringList NetInterfacePicker::allDevices()
{
    QStringList out;
    auto buf = fetchAdapters();
    if (buf.empty())
        return out;

    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    for (auto* a = addrs; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (a->OperStatus != IfOperStatusUp)
            continue;

        const QString name = QString::fromLatin1(a->AdapterName);
        const QString friendly = QString::fromWCharArray(a->FriendlyName);
        const QString desc = QString::fromWCharArray(a->Description);

        // 优先排除常见虚拟机/隧道网卡
        if (isVirtualDevice(friendly) || isVirtualDevice(desc))
            continue;

        out.append(name);
    }
    return out;
}

QString NetInterfacePicker::pickDevice()
{
    uint32_t ifIndex = 0;
    if (!bestRouteToInternet(ifIndex))
        defaultRouteFromTable(ifIndex);

    auto buf = fetchAdapters();
    if (buf.empty())
        return QString();

    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());

    // 1) 优先匹配默认路由所在网卡
    if (ifIndex != 0) {
        for (auto* a = addrs; a; a = a->Next) {
            if (a->IfIndex == ifIndex || a->Ipv6IfIndex == ifIndex)
                return QString::fromLatin1(a->AdapterName);
        }
    }

    // 2) 兜底：首个 Up 的非虚拟、非回环网卡
    for (auto* a = addrs; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (a->OperStatus != IfOperStatusUp)
            continue;
        const QString friendly = QString::fromWCharArray(a->FriendlyName);
        const QString desc = QString::fromWCharArray(a->Description);
        if (isVirtualDevice(friendly) || isVirtualDevice(desc))
            continue;
        return QString::fromLatin1(a->AdapterName);
    }

    return QString();
}

uint32_t NetInterfacePicker::ifIndexForDevice(const QString& deviceName)
{
    if (deviceName.isEmpty())
        return 0;

    // 如果已经是纯数字则直接转换
    bool ok = false;
    uint32_t parsed = deviceName.toUInt(&ok);
    if (ok)
        return parsed;

    auto buf = fetchAdapters();
    if (buf.empty())
        return 0;

    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    for (auto* a = addrs; a; a = a->Next) {
        if (deviceName.compare(QString::fromLatin1(a->AdapterName), Qt::CaseInsensitive) == 0)
            return a->IfIndex;
    }
    return 0;
}

QString NetInterfacePicker::deviceDescription(const QString& deviceName)
{
    if (deviceName.isEmpty())
        return QString();

    auto buf = fetchAdapters();
    if (buf.empty())
        return deviceName;

    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    for (auto* a = addrs; a; a = a->Next) {
        const QString name = QString::fromLatin1(a->AdapterName);
        const bool match = (deviceName.compare(name, Qt::CaseInsensitive) == 0)
                           || (deviceName.toUInt() == a->IfIndex);
        if (match) {
            const QString friendly = QString::fromWCharArray(a->FriendlyName);
            const QString desc = QString::fromWCharArray(a->Description);
            if (!desc.isEmpty() && desc != friendly)
                return QStringLiteral("%1 (%2)").arg(friendly, desc);
            return friendly;
        }
    }
    return deviceName;
}

bool NetInterfacePicker::isVirtualDevice(const QString& nameOrDescription)
{
    static const char* keys[] = {
        "loopback", "vmware", "virtualbox", "hyper-v", "openvpn", "tap-",
        "radmin", "bluetooth", "teredo", "isatap", "miniport", "wintun",
        "zerotier", "tailscale", "pseudo-interface", "sangfor", "expressvpn"
    };
    const QString v = nameOrDescription.toLower();
    for (const char* k : keys) {
        if (v.contains(QString::fromLatin1(k)))
            return true;
    }
    return false;
}
