#pragma once

#include <QString>
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

namespace ts {

class EtwPacketSource {
public:
    using PacketHandler = std::function<void(const uint8_t* packetBytes, uint32_t packetLen, uint32_t ifIndex)>;

    EtwPacketSource();
    ~EtwPacketSource();

    EtwPacketSource(const EtwPacketSource&) = delete;
    EtwPacketSource& operator=(const EtwPacketSource&) = delete;

    // 启动抓包会话。targetIfIndex 为 0 表示不过滤网卡（抓所有接口）
    bool start(uint32_t targetIfIndex, PacketHandler handler, QString& err);
    void stop();

    bool isRunning() const { return m_running.load(std::memory_order_relaxed); }
    void setFilterIfIndex(uint32_t ifIndex) { m_filterIfIndex.store(ifIndex, std::memory_order_relaxed); }
    uint32_t filterIfIndex() const { return m_filterIfIndex.load(std::memory_order_relaxed); }

    uint64_t totalEvents() const { return m_totalEvents.load(std::memory_order_relaxed); }
    uint64_t droppedEvents() const;

private:
    static void WINAPI onEventRecord(PEVENT_RECORD ev);
    void processEvent(PEVENT_RECORD ev);

    struct SessionProperties {
        EVENT_TRACE_PROPERTIES props;
        wchar_t loggerName[64];
    };

    std::atomic<bool> m_running{false};
    std::atomic<uint32_t> m_filterIfIndex{0};
    PacketHandler m_handler;

    SessionProperties* m_sessionProps = nullptr;
    TRACEHANDLE m_sessionHandle = 0;
    TRACEHANDLE m_consumerHandle = 0;
    EVENT_TRACE_LOGFILEW m_logFile{};
    std::thread m_consumerThread;

    std::atomic<uint64_t> m_totalEvents{0};
};

} // namespace ts
