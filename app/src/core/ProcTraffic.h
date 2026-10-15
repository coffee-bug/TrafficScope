#pragma once
#include "IpKey.h"

#include <QHash>
#include <QMutex>
#include <QReadWriteLock>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <cstdint>

// 连接 key（TsConnKey）与 IP key 见 IpKey.h —— 与 EtwSniffer 共用同一套四元组 key。

// 端口级短期记忆条目：pidFor 在"精确四元组 / 0.0.0.0 兜底"都查不到时的最后兜底。
// 短命连接（DNS、QUIC、秒进秒出的 HTTP）的生命周期可能整个落在两次连接表
// 快照之间；TCP 还额外覆盖 TIME_WAIT（连接已无主、但残留包仍在四元组上）的情况。
// 窗口见 ProcTraffic.cpp 的 kPortMemoTcpMs(120s) / kPortMemoUdpMs(15s)。
struct TsPortMemo {
    quint64 pid = 0;
    qint64 ms = 0;   // 最后一次见到该 (本机地址:端口) 的时间
};

// 进程流量归因。pidFor 的查询顺序：
//   1) ETW 实时映射（EtwSniffer::pidFor，连接建立/断开时由内核事件直接给出 PID）
//   2) 本拍连接表精确四元组（GetExtendedTcpTable / GetExtendedUdpTable，每行自带 PID）
//   3) 绑定 0.0.0.0 / :: 的 socket 兜底
//   4) 端口级短期记忆（TCP 120s / UDP 15s，见 ProcTraffic.cpp）
// 说明：连接表用普通用户权限即可读；IPv4 / IPv6 两张表都读，否则 IPv6 流量会全部
// 落入"未知进程"。快照频率由调用方驱动（当前 ~300ms，见 FloatWidget::m_procTimer）。
class ProcTraffic {
public:
    static ProcTraffic& instance();

    // 主线程周期调用（~300ms）：重建连接快照与端口记忆，
    // 并为新出现的 PID 解析进程名/路径
    void refresh();

    // 抓包线程调用。tcp=false 时按 (localIp, localPort) 匹配；
    // 绑定 0.0.0.0 / :: 的 socket 做兜底匹配；查不到返回 0（归入"未知进程"）
    quint64 pidFor(bool tcp, const IpKey& localIp, uint16_t localPort,
                   const IpKey& remoteIp, uint16_t remotePort) const;

    QString nameFor(quint64 pid) const;   // "msedge.exe"；pid=0 -> "未知流量"；未知 -> "未知进程"
    // pid=0 在界面里的名字（"未知流量"）：设置里可以关掉它的统计，历史页据此过滤
    static const QString& unattributedName();
    // 这个名字算不算"未知流量"：兼容改名前的历史库记录（那时候叫"未归因流量"）
    static bool isUnattributedName(const QString& name);
    QString pathFor(quint64 pid) const;   // exe 完整路径，用于取图标；可能为空
    // 程序类别："系统程序" / "第三方程序"（pid=0 的未归因流量返回空）。
    // 按 exe 路径归类（Windows 目录下算系统程序），**不做签名校验**：
    // 签名校验（WinVerifyTrust）每次都要读文件并联网核对吊销列表，成本远高于
    // 这里的路径判断，当前不引入。
    QString categoryFor(quint64 pid) const;
    // exe 版本资源里的 FileDescription（第二行描述，如 "Microsoft Edge"）；取不到返回空
    QString descFor(quint64 pid) const;

    // 该 PID 当前 TCP ESTABLISHED 连接数（详情弹窗展示用）
    int connCountFor(quint64 pid) const;

    // 同名进程的所有 PID（如多个 chrome.exe），供"结束同名全部进程"使用
    QVector<quint64> pidsForName(const QString& name) const;

    // 强制结束进程（TerminateProcess）。pid=4（System）会被 Windows 拒绝
    static bool terminateProcess(quint64 pid);
    // TCP 状态的可读名（ESTAB / TIME_WAIT ...），诊断日志用
    static QString tcpStateText(quint32 state);

    // 诊断：未归因 TCP 包在轮询快照里的连接状态分布（MIB_TCP_STATE_*）。
    // 用来判定 pid=0 的流量是否集中在 TIME_WAIT / CLOSE_WAIT 等终止状态。
    QString missStateReport() const;

private:
    ProcTraffic() = default;

    // 未归因样本：抓包线程在未命中时写入（有上限、去重），主线程在诊断里读取
    void rememberMiss(const TsConnKey& k) const;

    mutable QReadWriteLock m_lock;
    QHash<TsConnKey, quint64> m_tcpMap;
    QHash<TsConnKey, quint64> m_udpMap;
    mutable QMutex m_missLock;
    mutable QVector<TsConnKey> m_missSamples;   // 未归因样本（上限 64，去重）
    // (本机地址:端口) -> PID 的短期记忆，key 的远端部分恒为 0
    QHash<TsConnKey, TsPortMemo> m_tcpPortMemo;
    QHash<TsConnKey, TsPortMemo> m_udpPortMemo;
    QHash<quint64, int> m_connCount;       // pid -> TCP ESTABLISHED 数
    QHash<quint64, QString> m_nameCache;   // pid -> 进程名
    QHash<quint64, QString> m_pathCache;   // pid -> exe 路径
    QHash<quint64, QString> m_descCache;   // pid -> FileDescription
};
