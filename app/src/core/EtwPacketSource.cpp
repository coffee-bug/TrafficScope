#include "EtwPacketSource.h"

#include <cstring>
#include <vector>

namespace ts {

namespace {

static const GUID kNdisPacketCaptureGuid = {
    0x2ED6006E, 0x4729, 0x4609, {0xB4, 0x23, 0x3E, 0xE7, 0xBC, 0xD6, 0x78, 0xEF}
};
static constexpr wchar_t kSessionName[] = L"TrafficScopeCapture";

// 格式化系统错误码
QString formatWinError(DWORD code)
{
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   buf, sizeof(buf) - 1, nullptr);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return QString::fromLocal8Bit(s.c_str()) + QStringLiteral(" (代码: %1)").arg(code);
}

} // namespace

EtwPacketSource::EtwPacketSource() = default;

EtwPacketSource::~EtwPacketSource()
{
    stop();
}

bool EtwPacketSource::start(uint32_t targetIfIndex, PacketHandler handler, QString& err)
{
    if (m_running.load(std::memory_order_relaxed))
        stop();

    m_filterIfIndex.store(targetIfIndex, std::memory_order_relaxed);
    m_handler = std::move(handler);
    m_totalEvents.store(0, std::memory_order_relaxed);

    m_sessionProps = static_cast<SessionProperties*>(calloc(1, sizeof(SessionProperties)));
    if (!m_sessionProps) {
        err = QStringLiteral("内存不足分配 ETW 会话属性");
        return false;
    }

    m_sessionProps->props.Wnode.BufferSize = sizeof(SessionProperties);
    m_sessionProps->props.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    m_sessionProps->props.Wnode.ClientContext = 2; // SystemTime
    m_sessionProps->props.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    m_sessionProps->props.FlushTimer = 1;
    m_sessionProps->props.BufferSize = 16;         // 16KB 兼顾高吞吐与低延迟（实测 ~98ms）
    m_sessionProps->props.MinimumBuffers = 32;
    m_sessionProps->props.MaximumBuffers = 128;
    m_sessionProps->props.LoggerNameOffset = offsetof(SessionProperties, loggerName);

    // 尝试创建实时跟踪会话
    ULONG rc = StartTraceW(&m_sessionHandle, kSessionName, &m_sessionProps->props);
    if (rc == ERROR_ALREADY_EXISTS) {
        // 如果上次异常退出有残留会话，先注销后重建
        StopTraceW(0, kSessionName, &m_sessionProps->props);
        rc = StartTraceW(&m_sessionHandle, kSessionName, &m_sessionProps->props);
    }
    if (rc != ERROR_SUCCESS) {
        err = QStringLiteral("创建 ETW 抓包会话失败: ") + formatWinError(rc);
        free(m_sessionProps);
        m_sessionProps = nullptr;
        return false;
    }

    // 启用 Microsoft-Windows-NDIS-PacketCapture 提供程序
    rc = EnableTraceEx2(m_sessionHandle, &kNdisPacketCaptureGuid,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_VERBOSE, 0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        err = QStringLiteral("启用 NDIS 捕获提供程序失败: ") + formatWinError(rc);
        StopTraceW(m_sessionHandle, kSessionName, &m_sessionProps->props);
        free(m_sessionProps);
        m_sessionProps = nullptr;
        m_sessionHandle = 0;
        return false;
    }

    // 开启实时消费者
    m_logFile = {};
    m_logFile.LoggerName = const_cast<LPWSTR>(kSessionName);
    m_logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    m_logFile.EventRecordCallback = &EtwPacketSource::onEventRecord;
    m_logFile.Context = this;

    m_consumerHandle = OpenTraceW(&m_logFile);
    if (m_consumerHandle == INVALID_PROCESSTRACE_HANDLE || m_consumerHandle == 0) {
        err = QStringLiteral("打开 ETW 实时通道失败: ") + formatWinError(GetLastError());
        StopTraceW(m_sessionHandle, kSessionName, &m_sessionProps->props);
        free(m_sessionProps);
        m_sessionProps = nullptr;
        m_sessionHandle = 0;
        return false;
    }

    m_running.store(true, std::memory_order_relaxed);

    m_consumerThread = std::thread([this]() {
        // 提升抓包消费线程优先级，减少事件在内核缓冲区排队延迟
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        ProcessTrace(&m_consumerHandle, 1, nullptr, nullptr);
    });

    return true;
}

void EtwPacketSource::stop()
{
    if (!m_running.exchange(false, std::memory_order_relaxed))
        return;

    if (m_consumerHandle != 0 && m_consumerHandle != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(m_consumerHandle);
        m_consumerHandle = 0;
    }

    if (m_consumerThread.joinable())
        m_consumerThread.join();

    if (m_sessionHandle != 0 && m_sessionProps) {
        StopTraceW(m_sessionHandle, kSessionName, &m_sessionProps->props);
        m_sessionHandle = 0;
    }

    if (m_sessionProps) {
        free(m_sessionProps);
        m_sessionProps = nullptr;
    }
}

uint64_t EtwPacketSource::droppedEvents() const
{
    return static_cast<uint64_t>(m_logFile.EventsLost);
}

void WINAPI EtwPacketSource::onEventRecord(PEVENT_RECORD ev)
{
    if (!ev || !ev->UserContext)
        return;
    auto* self = static_cast<EtwPacketSource*>(ev->UserContext);
    self->processEvent(ev);
}

void EtwPacketSource::processEvent(PEVENT_RECORD ev)
{
    // 只处理 Microsoft-Windows-NDIS-PacketCapture 产生的事件
    if (!IsEqualGUID(ev->EventHeader.ProviderId, kNdisPacketCaptureGuid))
        return;

    // 1001 是实际的数据包传输事件
    if (ev->EventHeader.EventDescriptor.Id != 1001)
        return;

    // 校验头部长度与指针有效性
    if (!ev->UserData || ev->UserDataLength < 12)
        return;

    uint32_t hdr[3];
    std::memcpy(hdr, ev->UserData, sizeof(hdr));
    const uint32_t miniportIfIndex = hdr[0];
    const uint32_t fragSize = hdr[2];

    // 实测布局必须满足：12 字节头部 + fragSize 字节以太网帧 + 16 字节尾部 GFT 信息
    if (12ull + fragSize + 16ull != ev->UserDataLength)
        return;

    // 网卡过滤：如果设置了特定网卡，只保留匹配的 ifIndex
    const uint32_t targetIf = m_filterIfIndex.load(std::memory_order_relaxed);
    if (targetIf != 0 && miniportIfIndex != targetIf)
        return;

    m_totalEvents.fetch_add(1, std::memory_order_relaxed);

    if (m_handler) {
        const uint8_t* frameBytes = static_cast<const uint8_t*>(ev->UserData) + 12;
        m_handler(frameBytes, fragSize, miniportIfIndex);
    }
}

} // namespace ts
