#include "NativeHost.h"

#include "SiteResolver.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>

#include <atomic>
#include <thread>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace ts {

namespace {

#ifdef Q_OS_WIN
// 与主实例通信的管道名（与单实例守卫用的 "TrafficScope" 区分开）
const wchar_t* const kBridgePipeName = L"\\\\.\\pipe\\TrafficScopeSiteBridge";
#endif

// native messaging 单帧上限：Chrome 侧默认单条消息 1MB，这里留同样的量级，
// 超过就判为异常帧直接断开，不往主实例转。
const quint32 kMaxFrame = 1024 * 1024;

#ifdef Q_OS_WIN
// 逐字节读满 n：管道上是短读（short read）常态，ReadFile 一次不一定给满
bool readFully(HANDLE h, char* dst, quint32 n)
{
    quint32 got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(h, dst + got, DWORD(n - got), &r, nullptr) || r == 0)
            return false;
        got += quint32(r);
    }
    return true;
}

// 逐字节写满 n
bool writeFully(HANDLE h, const char* src, quint32 n)
{
    quint32 sent = 0;
    while (sent < n) {
        DWORD w = 0;
        if (!WriteFile(h, src + sent, DWORD(n - sent), &w, nullptr) || w == 0)
            return false;
        sent += quint32(w);
    }
    return true;
}

// 诊断走 stderr：Chrome 会把 host 的 stderr 原样收进扩展「服务工作进程」的控制台，
// 用户在 chrome://extensions 点开就能看到，是这条链路唯一的现场日志。
// 用 WriteFile 而不是 fprintf：GUI 子系统进程的 CRT stderr 可能没被初始化。
void logEvent(const char* event, int arg = -1)
{
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err == INVALID_HANDLE_VALUE || err == nullptr)
        return;
    char buf[128];
    int n = (arg >= 0)
        ? wsprintfA(buf, "{\"event\":\"%s\",\"arg\":%d}\n", event, arg)
        : wsprintfA(buf, "{\"event\":\"%s\"}\n", event);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(err, buf, DWORD(n), &written, nullptr);
    }
}
#endif

}  // namespace

struct SiteBridge::Impl {
#ifdef Q_OS_WIN
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::atomic<bool> stop{false};
    std::thread worker;

    ~Impl()
    {
        stopWork();
    }

    void stopWork()
    {
        if (stop.exchange(true))
            return;

        // 唤醒可能阻塞在 ConnectNamedPipe 的工作线程
        HANDLE wake = CreateFileW(
            kBridgePipeName,
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (wake != INVALID_HANDLE_VALUE) {
            CloseHandle(wake);
        }

        if (worker.joinable()) {
            worker.join();
        }

        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }
#endif
};

SiteBridge::SiteBridge() : m_impl(std::make_unique<Impl>())
{
}

SiteBridge::~SiteBridge()
{
}

SiteBridge& SiteBridge::instance()
{
    static SiteBridge s;
    return s;
}

bool SiteBridge::listen(QString* err)
{
#ifdef Q_OS_WIN
    if (m_impl->pipe != INVALID_HANDLE_VALUE)
        return true;

    // 必须开放 WorldAccess（空 DACL）：主实例提权运行（管理员令牌），而 host 由 Chrome 启动、
    // 拿的是同一用户的**过滤令牌**（无管理员资格）。命名管道默认 DACL 只给
    // 管理员 → 普通令牌 connect 会被拒（2026-10-12 实测踩到，现象是设置页
    // 永远显示"未连接"）。管道里只有"域名/IP → 站点"映射，无敏感数据，
    // 开放可读写风险可接受。
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    m_impl->pipe = CreateNamedPipeW(
        kBridgePipeName,
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        65536,
        65536,
        1000,
        &sa
    );

    if (m_impl->pipe == INVALID_HANDLE_VALUE) {
        if (err) {
            *err = QString::asprintf("CreateNamedPipeW failed: %lu", GetLastError());
        }
        return false;
    }

    m_impl->stop = false;
    m_impl->worker = std::thread([this]() {
        while (!m_impl->stop.load()) {
            BOOL connected = ConnectNamedPipe(m_impl->pipe, nullptr)
                ? TRUE
                : (GetLastError() == ERROR_PIPE_CONNECTED);

            if (m_impl->stop.load())
                break;

            if (connected) {
                // 循环读取直到客户端断开 (ERROR_BROKEN_PIPE)
                QByteArray body;
                char buf[4096];
                DWORD bytesRead = 0;
                while (ReadFile(m_impl->pipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                    body.append(buf, int(bytesRead));
                    if (quint32(body.size()) > kMaxFrame) {
                        body.clear();
                        break;
                    }
                }

                if (!body.isEmpty()) {
                    // 跨线程调度回 Qt 主线程处理，保证 SiteResolver 线程安全和状态更新
                    QMetaObject::invokeMethod(QCoreApplication::instance(), [this, body]() {
                        const int n = SiteResolver::instance().feedBrowserJson(body);
                        if (n > 0) {
                            m_lastFeedMs = QDateTime::currentMSecsSinceEpoch();
                            m_lastBatchItems = n;
                            m_totalItems += quint64(n);
                        }
                    }, Qt::QueuedConnection);
                }

                DisconnectNamedPipe(m_impl->pipe);
            } else {
                if (m_impl->stop.load())
                    break;
                Sleep(50);
            }
        }
    });

    return true;
#else
    if (err)
        *err = QStringLiteral("Native host bridge only supported on Windows");
    return false;
#endif
}

qint64 SiteBridge::lastFeedMs() const { return m_lastFeedMs; }
int SiteBridge::lastBatchItems() const { return m_lastBatchItems; }
quint64 SiteBridge::totalItems() const { return m_totalItems; }

int runNativeHost()
{
#ifdef Q_OS_WIN
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE || in == nullptr)
        return 2;   // 不是被浏览器拉起来的（直接双击了 host exe）

    for (;;) {
        quint32 len = 0;
        // 前 4 字节是长度（native byte order，Windows 即小端）
        if (!readFully(in, reinterpret_cast<char*>(&len), 4))
            return 0;                       // stdin 关闭 = 浏览器断开，正常退出
        if (len == 0 || len > kMaxFrame)
            return 1;

        QByteArray body(int(len), Qt::Uninitialized);
        if (!readFully(in, body.data(), len))
            return 0;

        // 转发给主实例；主实例没跑就算了（扩展会定时重试）
        HANDLE pipe = CreateFileW(
            kBridgePipeName,
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (pipe == INVALID_HANDLE_VALUE) {
            logEvent("bridge_connect_failed");
            continue;
        }

        if (writeFully(pipe, body.constData(), quint32(body.size()))) {
            FlushFileBuffers(pipe);
            logEvent("forwarded", int(body.size()));
        } else {
            logEvent("bridge_write_failed");
        }
        CloseHandle(pipe);
        // 不回 ack：扩展不读回包，省一次往返
    }
#else
    return 3;   // Native Messaging host 目前只在 Windows 上实现
#endif
}

}  // namespace ts
