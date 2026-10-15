#include "EtwSniffer.h"
#include "ProcTraffic.h"
#include "Settings.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QString>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>   // ntohs / ntohl（必须排在 windows.h 之前）
#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>
#include <tdh.h>

#include <atomic>
#include <share.h>
#include <stdio.h>
#include <vector>

namespace {

// Microsoft-Windows-TCPIP：Win8+ 的 TCP/UDP 事件都在这里，且自带 PID。
// 不要改用 Microsoft-Windows-Kernel-Network —— 实测它收不到可消费事件。
const GUID kTcpipGuid =
    {0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xEF, 0x9D, 0x7B, 0xA2, 0x82, 0x18, 0x8A}};
const wchar_t* const kSessionName = L"TrafficScopeEtw";
const int kSampleLimit = 40;   // 明细样本上限（验证字段用）

FILE* g_log = nullptr;
QMutex g_lock;
int g_total = 0;
int g_dumped = 0;
int g_tcpipEvents = 0;         // TCPIP provider 的事件数
QHash<quint32, int> g_byId;    // EventID -> 次数
QSet<quint32> g_dumpedIds;     // 已 dump 过的事件类型（每种一条样本）

wchar_t g_loggerName[128] = {0};
EVENT_TRACE_LOGFILEW g_logFile = {};
TRACEHANDLE g_session = 0;
TRACEHANDLE g_trace = INVALID_PROCESSTRACE_HANDLE;
HANDLE g_thread = nullptr;

// ---- 实时映射表：由 ETW 事件维护的「四元组 → PID」 ----
// ProcessTrace 是单线程回调，表只在该线程写；pidFor 由抓包线程读，故加锁。
struct MapEntry {
    quint64 pid = 0;
    qint64 ms = 0;   // 最后一次写入的时刻，供过期清理用
};
QMutex g_mapLock;
QHash<TsConnKey, MapEntry> g_tcpMap;
QHash<TsConnKey, MapEntry> g_udpMap;
int g_tcpAdds = 0;   // 诊断
int g_udpAdds = 0;

// 过期清理：ETW 只在"事件发生那一刻"给 PID，而断开事件可能收不到（事件丢失、进程崩溃、
// 连接被强制结束），只靠 forgetTcp 收敛的话表会无限增长。所以给每条记录一个 TTL。
// 淘汰是安全的：pidFor 只是**第一级**查询，后面还有 300ms 轮询快照与端口记忆兜底
// （见 ProcTraffic::pidFor），长连接被淘汰后只是退回兜底路径，不会丢归因。
const qint64 kTcpEntryTtlMs = 10 * 60 * 1000;   // TCP：断开事件通常会有，给足余量
const qint64 kUdpEntryTtlMs = 5 * 60 * 1000;    // UDP：没有断开事件，只能靠收发刷新
const qint64 kPruneIntervalMs = 30 * 1000;      // 清理本身也节流，别每个事件都扫表
qint64 g_lastPruneMs = 0;

void pruneMap(QHash<TsConnKey, MapEntry>& m, qint64 now, qint64 ttl)
{
    for (auto it = m.begin(); it != m.end(); ) {
        if (now - it->ms > ttl)
            it = m.erase(it);
        else
            ++it;
    }
}

// 由写入路径驱动（此时已持有 g_mapLock），最多 kPruneIntervalMs 扫一次
void pruneMaps(qint64 now)
{
    if (now - g_lastPruneMs < kPruneIntervalMs)
        return;
    g_lastPruneMs = now;
    pruneMap(g_tcpMap, now, kTcpEntryTtlMs);
    pruneMap(g_udpMap, now, kUdpEntryTtlMs);
}
std::atomic<quint64> g_etwHits{0};   // pidFor 命中 ETW 映射的次数（诊断）

// 未归因包按协议分类（诊断）：回答"pid=0 的流量集中在 UDP 还是 TCP"
std::atomic<quint64> g_missTcpPkts{0};
std::atomic<quint64> g_missTcpBytes{0};
std::atomic<quint64> g_missUdpPkts{0};
std::atomic<quint64> g_missUdpBytes{0};
std::atomic<quint64> g_missOtherPkts{0};
std::atomic<quint64> g_missOtherBytes{0};

// 按属性名取原始字节
bool propRaw(PEVENT_RECORD ev, PTRACE_EVENT_INFO info, const wchar_t* name,
             std::vector<BYTE>& out)
{
    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        const EVENT_PROPERTY_INFO& pi = info->EventPropertyInfoArray[i];
        const wchar_t* nm = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const BYTE*>(info) + pi.NameOffset);
        if (wcscmp(nm, name) != 0)
            continue;

        PROPERTY_DATA_DESCRIPTOR desc;
        desc.PropertyName = reinterpret_cast<ULONG_PTR>(nm);
        desc.ArrayIndex = ULONG_MAX;
        desc.Reserved = 0;

        ULONG sz = 0;
        if (TdhGetPropertySize(ev, 0, nullptr, 1, &desc, &sz) != ERROR_SUCCESS || sz == 0)
            return false;
        out.assign(sz, 0);
        return TdhGetProperty(ev, 0, nullptr, 1, &desc, sz, out.data()) == ERROR_SUCCESS;
    }
    return false;
}

bool propU32(PEVENT_RECORD ev, PTRACE_EVENT_INFO info, const wchar_t* name, quint32& out)
{
    std::vector<BYTE> v;
    if (!propRaw(ev, info, name, v) || v.size() < 4)
        return false;
    out = *reinterpret_cast<const quint32*>(v.data());
    return true;
}

// TDI 地址：16 字节 = IPv4、28 字节 = IPv6；端口在头 2 字节（网络序）
bool tdiAddr(const std::vector<BYTE>& v, IpKey& ip, quint16& port)
{
    if (v.size() < 16)
        return false;
    port = ntohs(*reinterpret_cast<const quint16*>(v.data()));
    if (v.size() >= 28)
        ip = IpKey::fromV6(v.data() + 6);
    else
        ip = IpKey::fromV4(ntohl(*reinterpret_cast<const quint32*>(v.data() + 2)));
    return true;
}

// sockaddr：16 字节 = sockaddr_in、28 字节 = sockaddr_in6
bool sockAddr(const std::vector<BYTE>& v, IpKey& ip, quint16& port)
{
    if (v.size() < 16)
        return false;
    port = ntohs(*reinterpret_cast<const quint16*>(v.data() + 2));
    if (v.size() >= 28)
        ip = IpKey::fromV6(v.data() + 8);
    else
        ip = IpKey::fromV4(ntohl(*reinterpret_cast<const quint32*>(v.data() + 4)));
    return true;
}

// TCP 连接建立：ProcessId + 地址 → 写入映射
void rememberTcp(PEVENT_RECORD ev, PTRACE_EVENT_INFO info)
{
    quint32 pid = 0;
    if (!propU32(ev, info, L"ProcessId", pid) || pid == 0) {
        // 1300（TcpTcb）用 "Pid"，1044/1034 用 "ProcessId"
        if (!propU32(ev, info, L"Pid", pid) || pid == 0)
            return;
    }

    std::vector<BYTE> la, ra;
    if (!propRaw(ev, info, L"LocalAddress", la) || !propRaw(ev, info, L"RemoteAddress", ra))
        return;

    // 该字段可能是 TDI 地址、也可能直接是 sockaddr：两者长度相同但字段偏移不同，
    // 因此两种解释都记一份，抓包侧用哪种都能命中。
    TsConnKey keys[2];
    int n = 0;
    IpKey lip, rip;
    quint16 lp = 0, rp = 0;
    if (tdiAddr(la, lip, lp) && tdiAddr(ra, rip, rp)) {
        keys[n].localAddr = lip;
        keys[n].localPort = lp;
        keys[n].remoteAddr = rip;
        keys[n].remotePort = rp;
        ++n;
    }
    if (sockAddr(la, lip, lp) && sockAddr(ra, rip, rp)) {
        keys[n].localAddr = lip;
        keys[n].localPort = lp;
        keys[n].remoteAddr = rip;
        keys[n].remotePort = rp;
        ++n;
    }
    if (n == 0)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker l(&g_mapLock);
    for (int i = 0; i < n; ++i) {
        MapEntry& e = g_tcpMap[keys[i]];
        e.pid = pid;
        e.ms = now;
    }
    ++g_tcpAdds;
    pruneMaps(now);
}

// TCP 断开 / 状态收敛：移除映射（两种解释都清）
void forgetTcp(PEVENT_RECORD ev, PTRACE_EVENT_INFO info)
{
    std::vector<BYTE> la, ra;
    if (!propRaw(ev, info, L"LocalSockAddr", la)
        || !propRaw(ev, info, L"RemoteSockAddr", ra)) {
        if (!propRaw(ev, info, L"LocalAddress", la)
            || !propRaw(ev, info, L"RemoteAddress", ra))
            return;
    }

    IpKey lip, rip;
    quint16 lp = 0, rp = 0;
    QMutexLocker l(&g_mapLock);
    if (tdiAddr(la, lip, lp) && tdiAddr(ra, rip, rp)) {
        TsConnKey k;
        k.localAddr = lip;
        k.localPort = lp;
        k.remoteAddr = rip;
        k.remotePort = rp;
        g_tcpMap.remove(k);
    }
    if (sockAddr(la, lip, lp) && sockAddr(ra, rip, rp)) {
        TsConnKey k;
        k.localAddr = lip;
        k.localPort = lp;
        k.remoteAddr = rip;
        k.remotePort = rp;
        g_tcpMap.remove(k);
    }
}

// UDP 收发：Pid + 本机 sockaddr → 写入映射（只按本机端，与 pidFor 一致）
void rememberUdp(PEVENT_RECORD ev, PTRACE_EVENT_INFO info)
{
    quint32 pid = 0;
    if (!propU32(ev, info, L"Pid", pid) || pid == 0)
        return;
    std::vector<BYTE> la;
    if (!propRaw(ev, info, L"LocalSockAddr", la))
        return;
    IpKey lip;
    quint16 lp = 0;
    if (!sockAddr(la, lip, lp))
        return;

    TsConnKey k;
    k.localAddr = lip;
    k.localPort = lp;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker l(&g_mapLock);
    MapEntry& e = g_udpMap[k];
    e.pid = pid;
    e.ms = now;
    ++g_udpAdds;
    pruneMaps(now);
}

// 只处理这几类事件（其余事件连 TDH 解析都不做，降低开销）
bool interestingId(quint32 id)
{
    switch (id) {
    case 1300:   // TcpTcb：连接建立，带 Pid + 完整 sockaddr（主力来源）
    case 1044:   // TCP connect / accept（带 ProcessId）
    case 1034:
    case 1479:   // TCP disconnect
    case 1214:
    case 1186:   // TCP 状态变化
    case 1183:
    case 1169:   // UDP send / recv
    case 1170:
        return true;
    default:
        return false;
    }
}

void handleEtwEvent(PEVENT_RECORD ev, PTRACE_EVENT_INFO info)
{
    switch (ev->EventHeader.EventDescriptor.Id) {
    case 1300:
    case 1044:
    case 1034:
        rememberTcp(ev, info);
        break;
    case 1479:
    case 1214:
    case 1186:
    case 1183:
        forgetTcp(ev, info);
        break;
    case 1169:
    case 1170:
        rememberUdp(ev, info);
        break;
    default:
        break;
    }
}

void logLine(const QString& s)
{
    QMutexLocker l(&g_lock);
    if (!g_log)
        return;
    const QByteArray utf8 = s.toUtf8();
    fwrite(utf8.constData(), 1, size_t(utf8.size()), g_log);
    fputc('\n', g_log);
    fflush(g_log);
}

void dumpEvent(PEVENT_RECORD ev)
{
    ULONG infoSize = 0;
    TdhGetEventInformation(ev, 0, nullptr, nullptr, &infoSize);
    if (infoSize == 0)
        return;
    std::vector<BYTE> buf(infoSize);
    PTRACE_EVENT_INFO info = reinterpret_cast<PTRACE_EVENT_INFO>(buf.data());
    if (TdhGetEventInformation(ev, 0, nullptr, info, &infoSize) != ERROR_SUCCESS)
        return;

    const wchar_t* evName = L"";
    if (info->EventNameOffset)
        evName = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const BYTE*>(info) + info->EventNameOffset);

    logLine(QStringLiteral("EVENT id=%1 name=%2 props=%3")
                .arg(ev->EventHeader.EventDescriptor.Id)
                .arg(QString::fromWCharArray(evName))
                .arg(info->TopLevelPropertyCount));

    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        const EVENT_PROPERTY_INFO& pi = info->EventPropertyInfoArray[i];
        const wchar_t* name = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const BYTE*>(info) + pi.NameOffset);

        PROPERTY_DATA_DESCRIPTOR desc;
        desc.PropertyName = reinterpret_cast<ULONG_PTR>(name);
        desc.ArrayIndex = ULONG_MAX;
        desc.Reserved = 0;

        ULONG sz = 0;
        if (TdhGetPropertySize(ev, 0, nullptr, 1, &desc, &sz) != ERROR_SUCCESS)
            continue;
        std::vector<BYTE> v(sz ? sz : 8, 0);
        if (TdhGetProperty(ev, 0, nullptr, 1, &desc, sz, v.data()) != ERROR_SUCCESS)
            continue;

        const USHORT t = pi.nonStructType.InType;
        QString val;
        if (t == TDH_INTYPE_UNICODESTRING)
            val = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(v.data()));
        else if (t == TDH_INTYPE_UINT32)
            val = QString::number(*reinterpret_cast<const UINT32*>(v.data()));
        else if (t == TDH_INTYPE_UINT64)
            val = QString::number(*reinterpret_cast<const unsigned long long*>(v.data()));
        else if (t == TDH_INTYPE_INT32)
            val = QString::number(*reinterpret_cast<const INT32*>(v.data()));
        else if (t == TDH_INTYPE_BINARY) {
            QString hex;
            for (size_t j = 0; j < v.size() && j < 32; ++j)
                hex += QStringLiteral("%1").arg(uint(v[j]), 2, 16, QLatin1Char('0'));
            val = QStringLiteral("<bin %1>").arg(hex);
        } else {
            val = QStringLiteral("<type %1, %2 bytes>").arg(int(t)).arg(sz);
        }

        logLine(QStringLiteral("    %1 = %2").arg(QString::fromWCharArray(name), val));
    }
}

VOID WINAPI onEvent(PEVENT_RECORD ev)
{
    // 只订阅了 TCPIP 一个 provider，其它来源的事件直接忽略
    if (memcmp(&ev->EventHeader.ProviderId, &kTcpipGuid, sizeof(GUID)) != 0)
        return;
    {
        QMutexLocker l(&g_lock);
        ++g_total;
        g_byId[ev->EventHeader.EventDescriptor.Id] += 1;
        ++g_tcpipEvents;
    }
    // ① 实时映射：对所有关心的事件生效，不受取样限制
    if (interestingId(ev->EventHeader.EventDescriptor.Id)) {
        ULONG infoSize = 0;
        TdhGetEventInformation(ev, 0, nullptr, nullptr, &infoSize);
        if (infoSize > 0) {
            std::vector<BYTE> buf(infoSize);
            PTRACE_EVENT_INFO info = reinterpret_cast<PTRACE_EVENT_INFO>(buf.data());
            if (TdhGetEventInformation(ev, 0, nullptr, info, &infoSize) == ERROR_SUCCESS)
                handleEtwEvent(ev, info);
        }
    }

    // ② 诊断取样：每种事件类型只留一条样本
    {
        QMutexLocker l(&g_lock);
        if (g_dumpedIds.contains(ev->EventHeader.EventDescriptor.Id))
            return;
        if (g_dumped >= kSampleLimit)
            return;
        g_dumpedIds.insert(ev->EventHeader.EventDescriptor.Id);
        ++g_dumped;
    }
    dumpEvent(ev);
}

DWORD WINAPI traceThread(LPVOID p)
{
    TRACEHANDLE h = *reinterpret_cast<TRACEHANDLE*>(p);
    const ULONG rc = ProcessTrace(&h, 1, nullptr, nullptr);
    logLine(QStringLiteral("ProcessTrace exited rc=%1 total=%2").arg(rc).arg(g_total));
    return rc;
}

void stopSessionProps(std::vector<BYTE>& buf)
{
    ULONG bufSize = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
    buf.assign(bufSize, 0);
    PEVENT_TRACE_PROPERTIES props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
    props->Wnode.BufferSize = bufSize;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
}

}  // namespace

EtwSniffer& EtwSniffer::instance()
{
    static EtwSniffer s;
    return s;
}

bool EtwSniffer::probeEnabled()
{
    // 默认值：Debug 构建开、Release 关；环境变量 TRAFFICSCOPE_ETW_PROBE 优先级最高，
    // 便于在交付版（Release）上按需打开诊断输出。
    static const bool on = []() {
        const QByteArray v = qgetenv("TRAFFICSCOPE_ETW_PROBE");
        if (!v.isEmpty())
            return v != "0";
#ifdef QT_DEBUG
        return true;
#else
        return false;
#endif
    }();
    return on;
}

bool EtwSniffer::start(QString* err)
{
    if (m_running)
        return true;

    // 诊断日志默认关闭（只在调试时开启）：关闭时 g_log 保持 nullptr，
    // logLine / dumpStats 自动空转，交付版不会在 data/ 下产生日志文件。
    // 注意：日志建不出来不再让整个会话启动失败（否则一个诊断输出会拖垮归因功能）。
    if (probeEnabled()) {
        const QString path = ts::dataDir() + QStringLiteral("/etw_probe.log");
        // _SH_DENYNO：允许其它进程读取，运行中也能看日志
        g_log = _wfsopen(reinterpret_cast<const wchar_t*>(path.utf16()), L"wb", _SH_DENYNO);
        if (g_log) {
            setvbuf(g_log, nullptr, _IONBF, 0);
            logLine(QStringLiteral("=== EtwSniffer start ==="));
        }
    }

    ULONG bufSize = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
    std::vector<BYTE> propsBuf(bufSize, 0);
    PEVENT_TRACE_PROPERTIES props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(propsBuf.data());
    props->Wnode.BufferSize = bufSize;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->BufferSize = 512;
    props->MinimumBuffers = 32;
    props->MaximumBuffers = 64;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    // 清理可能残留的同名会话（上次异常退出）。这里按尽最大努力处理：
    // 没有残留会话时本来就返回 ERROR_WMI_INSTANCE_NOT_FOUND，不算错误。
    ControlTraceW(0, const_cast<LPWSTR>(kSessionName), props, EVENT_TRACE_CONTROL_STOP);

    ULONG rc = StartTraceW(&g_session, kSessionName, props);
    logLine(QStringLiteral("StartTrace rc=%1").arg(rc));
    if (rc != ERROR_SUCCESS) {
        logLine(QStringLiteral("StartTrace FAILED - 需要管理员权限"));
        if (err)
            *err = QStringLiteral("StartTrace rc=%1").arg(rc);
        return false;
    }

    // 关键字全量：TCPIP provider 的关键字位没有公开文档，实测全量下有 39 种 EventID，
    // 收敛到某个子集会静默丢掉要用的事件；开销由 interestingId() 在消费端过滤。
    const ULONG rcTcp = EnableTraceEx2(g_session, &kTcpipGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                       TRACE_LEVEL_INFORMATION, 0xFFFFFFFFFFFFFFFFULL, 0, 0, nullptr);
    logLine(QStringLiteral("EnableTraceEx2(tcpip) rc=%1").arg(rcTcp));
    if (rcTcp != ERROR_SUCCESS)
        logLine(QStringLiteral("EnableTraceEx2(tcpip) FAILED rc=%1：ETW 归因不可用，回退轮询快照")
                    .arg(rcTcp));

    wcscpy_s(g_loggerName, kSessionName);
    ZeroMemory(&g_logFile, sizeof(g_logFile));
    g_logFile.LoggerName = g_loggerName;   // 必须可写：OpenTrace 可能改写它
    g_logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    g_logFile.EventRecordCallback = onEvent;

    g_trace = OpenTraceW(&g_logFile);
    logLine(QStringLiteral("OpenTrace=%1").arg(quint64(g_trace)));
    if (g_trace == INVALID_PROCESSTRACE_HANDLE) {
        if (err)
            *err = QStringLiteral("OpenTrace failed");
        std::vector<BYTE> stopBuf;
        stopSessionProps(stopBuf);
        ControlTraceW(0, const_cast<LPWSTR>(kSessionName),
                      reinterpret_cast<PEVENT_TRACE_PROPERTIES>(stopBuf.data()),
                      EVENT_TRACE_CONTROL_STOP);
        return false;
    }

    g_thread = CreateThread(nullptr, 0, traceThread, &g_trace, 0, nullptr);
    if (!g_thread) {
        // 线程没起来 → ProcessTrace 永远不会执行、映射表永远不填充。
        // 必须把刚建好的会话收干净再退出，不能留在"半启动"状态。
        const DWORD le = GetLastError();
        logLine(QStringLiteral("CreateThread FAILED err=%1").arg(le));
        if (err)
            *err = QStringLiteral("CreateThread failed: %1").arg(le);
        CloseTrace(g_trace);
        g_trace = INVALID_PROCESSTRACE_HANDLE;
        std::vector<BYTE> stopBuf;
        stopSessionProps(stopBuf);
        ControlTraceW(0, const_cast<LPWSTR>(kSessionName),
                      reinterpret_cast<PEVENT_TRACE_PROPERTIES>(stopBuf.data()),
                      EVENT_TRACE_CONTROL_STOP);
        return false;
    }
    m_running = true;
    logLine(QStringLiteral("consumer thread started"));
    return true;
}

void EtwSniffer::stop()
{
    if (!m_running)
        return;

    CloseTrace(g_trace);
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    std::vector<BYTE> stopBuf;
    stopSessionProps(stopBuf);
    ControlTraceW(0, const_cast<LPWSTR>(kSessionName),
                  reinterpret_cast<PEVENT_TRACE_PROPERTIES>(stopBuf.data()),
                  EVENT_TRACE_CONTROL_STOP);

    logLine(QStringLiteral("=== stop: total=%1 dumped=%2 ===").arg(g_total).arg(g_dumped));

    QHash<quint32, int> snapshot;
    {
        QMutexLocker l(&g_lock);
        snapshot = g_byId;
    }
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
        logLine(QStringLiteral("EventID %1 : %2").arg(it.key()).arg(it.value()));

    {
        QMutexLocker l(&g_lock);
        if (g_log) {
            fclose(g_log);
            g_log = nullptr;
        }
    }
    m_running = false;
}

void EtwSniffer::dumpStats()
{
    QMutexLocker l(&g_lock);
    if (!g_log)
        return;

    QString ids;
    for (auto it = g_byId.constBegin(); it != g_byId.constEnd(); ++it)
        ids += QStringLiteral(" %1:%2").arg(it.key()).arg(it.value());

    int tcpN = 0;
    int udpN = 0;
    int tcpAdds = 0;
    int udpAdds = 0;
    {
        QMutexLocker ml(&g_mapLock);
        tcpN = g_tcpMap.size();
        udpN = g_udpMap.size();
        tcpAdds = g_tcpAdds;
        udpAdds = g_udpAdds;
    }

    const QString line =
        QStringLiteral("STATS total=%1 tcpip=%2 dumped=%3 tcpMap=%4 udpMap=%5 adds=%6/%7 etwHits=%8 ids=%9")
            .arg(g_total)
            .arg(g_tcpipEvents)
            .arg(g_dumped)
            .arg(tcpN)
            .arg(udpN)
            .arg(tcpAdds)
            .arg(udpAdds)
            .arg(quint64(g_etwHits.load(std::memory_order_relaxed)))
            .arg(ids);
    const QByteArray utf8 = line.toUtf8();
    fwrite(utf8.constData(), 1, size_t(utf8.size()), g_log);
    fputc('\n', g_log);

    // 未归因包按协议分类：回答"pid=0 的流量集中在 UDP 还是 TCP"
    const QString miss = QStringLiteral("MISS tcp=%1/%2B udp=%3/%4B other=%5/%6B")
                             .arg(quint64(g_missTcpPkts.load(std::memory_order_relaxed)))
                             .arg(quint64(g_missTcpBytes.load(std::memory_order_relaxed)))
                             .arg(quint64(g_missUdpPkts.load(std::memory_order_relaxed)))
                             .arg(quint64(g_missUdpBytes.load(std::memory_order_relaxed)))
                             .arg(quint64(g_missOtherPkts.load(std::memory_order_relaxed)))
                             .arg(quint64(g_missOtherBytes.load(std::memory_order_relaxed)));
    const QByteArray missUtf8 = miss.toUtf8();
    fwrite(missUtf8.constData(), 1, size_t(missUtf8.size()), g_log);
    fputc('\n', g_log);
    fflush(g_log);

    // 未归因样本对应的连接状态分布（验证是否集中在 TIME_WAIT 等终止状态）
    const QString states =
        QStringLiteral("MISS_STATE ") + ProcTraffic::instance().missStateReport();
    const QByteArray stUtf8 = states.toUtf8();
    fwrite(stUtf8.constData(), 1, size_t(stUtf8.size()), g_log);
    fputc('\n', g_log);
    fflush(g_log);
}

void EtwSniffer::noteUnattributed(bool tcp, bool udp, quint64 bytes)
{
    if (tcp) {
        g_missTcpPkts.fetch_add(1, std::memory_order_relaxed);
        g_missTcpBytes.fetch_add(bytes, std::memory_order_relaxed);
    } else if (udp) {
        g_missUdpPkts.fetch_add(1, std::memory_order_relaxed);
        g_missUdpBytes.fetch_add(bytes, std::memory_order_relaxed);
    } else {
        g_missOtherPkts.fetch_add(1, std::memory_order_relaxed);
        g_missOtherBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

quint64 EtwSniffer::pidFor(bool tcp, const IpKey& localIp, uint16_t localPort,
                           const IpKey& remoteIp, uint16_t remotePort) const
{
    TsConnKey k;
    k.localAddr = localIp;
    k.localPort = localPort;
    k.remoteAddr = tcp ? remoteIp : IpKey();
    k.remotePort = tcp ? remotePort : 0;

    QMutexLocker l(&g_mapLock);
    const QHash<TsConnKey, MapEntry>& m = tcp ? g_tcpMap : g_udpMap;
    auto it = m.constFind(k);
    if (it != m.constEnd() && it->pid != 0) {
        g_etwHits.fetch_add(1, std::memory_order_relaxed);
        return it->pid;
    }
    return 0;
}
