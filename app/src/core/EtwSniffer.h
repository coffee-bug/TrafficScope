#pragma once
#include "IpKey.h"

#include <QString>
#include <QtGlobal>
#include <cstdint>

// ETW 归因：订阅内核 TCP/IP provider（连接建立/断开、UDP 收发事件自带 PID）。
//
// 定位：已接入 ProcTraffic::pidFor 作为第一级查询（ETW 实时映射 → 轮询快照兜底），
// TCP 连接建立/断开时由内核事件直接给出 PID，没有采样间隙，短命连接也能命中。
//
// 说明：
// - 需要管理员权限（ETW 会话），与抓包要求一致；
// - 纯 Win32（ETW + TDH），无第三方依赖、无内核驱动、无需签名；
// - 诊断输出（用户数据目录下的 data/etw_probe.log）**默认关闭**，只在调试时开启，
//   见 probeEnabled()；关闭时不会创建该文件，logLine/dumpStats 均为空转。
class EtwSniffer {
public:
    static EtwSniffer& instance();

    // 诊断日志开关：Debug 构建默认开启、Release 默认关闭；
    // 两种构建都可用环境变量强制覆盖（TRAFFICSCOPE_ETW_PROBE=1 开 / =0 关）。
    static bool probeEnabled();

    // 开启会话并在后台线程消费；失败时通过 err 返回原因（不影响主流程）
    bool start(QString* err = nullptr);
    void stop();

    // 主线程定时调用：把"已收事件数 / EventID 分布"写进诊断日志。
    // 仅在 probeEnabled() 为真时才有意义，否则立即返回。
    void dumpStats();

    // 实时映射：由 ETW 事件维护的「四元组 → PID」，未命中返回 0。
    // 抓包线程高频调用，内部只做读锁 + 哈希查找。
    quint64 pidFor(bool tcp, const IpKey& localIp, uint16_t localPort,
                   const IpKey& remoteIp, uint16_t remotePort) const;

    // 诊断：记录一个"未归因"的包（pidFor 返回 0 时由 PacketSource 调用），
    // 按 TCP / UDP / 其它（ICMP、IPv6 控制协议等无端口流量）分类累计。
    void noteUnattributed(bool tcp, bool udp, quint64 bytes);

private:
    EtwSniffer() = default;
    ~EtwSniffer() = default;

    bool m_running = false;
};
