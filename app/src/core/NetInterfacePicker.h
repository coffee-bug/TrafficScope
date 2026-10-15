#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>

// 网卡自动选择与枚举：基于纯系统 API（GetAdaptersAddresses / GetBestRoute2），
// 摆脱对 Npcap pcap_findalldevs 的依赖。
// 设备标识符统一采用 AdapterName（GUID 格式，如 "{C14E13C1-...}"），
// 并提供向 ifIndex 的无损双向转换。
class NetInterfacePicker {
public:
    // 返回所有可用的网络适配器标识符（AdapterName GUID）列表
    static QStringList allDevices();

    // 返回当前上网默认路由所绑定的网卡标识符；失败返回空串
    static QString pickDevice();

    // 将网卡标识符解析为对应的接口索引（ifIndex）；若未找到返回 0
    static uint32_t ifIndexForDevice(const QString& deviceName);

    // 网卡标识符 -> 可读网卡描述（友好名称 + 驱动硬件描述）
    // 例如："以太网 (Realtek PCIe GbE Family Controller)"
    static QString deviceDescription(const QString& deviceName);

    // 名称/描述是否属于虚拟网卡（VMware / VirtualBox / TAP / 隧道等）
    static bool isVirtualDevice(const QString& nameOrDescription);

private:
    static bool bestRouteToInternet(uint32_t& ifIndex);
    static bool defaultRouteFromTable(uint32_t& ifIndex);
    static uint32_t adapterIpv4ByIndex(uint32_t ifIndex);
};
