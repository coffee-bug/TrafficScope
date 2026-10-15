#pragma once
#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <functional>
#include <string>
#include <thread>
#include <atomic>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ts {

// 单实例保护：用 Windows 原生命名管道做跨进程互斥与唤醒，
// 摆脱对 QtNetwork 模块（QLocalServer/QLocalSocket）的依赖。
//
// 机制：
//   1. 尝试连接 \\.\pipe\<name>；若连上，发送 "show\n" 并退出（唤醒已有实例）；
//   2. 连不上时创建该管道（带空 DACL 保证权限兼容），后台线程监听连接，
//      收到 "show" 时通过 QMetaObject::invokeMethod 唤醒主界面。
class SingleInstance {
public:
    using ActivateCallback = std::function<void()>;

    SingleInstance() = default;
    ~SingleInstance() { stop(); }

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool tryStart(const QString& name, QString* err = nullptr) {
#ifdef Q_OS_WIN
        m_pipePath = (QStringLiteral(R"(\\.\pipe\)") + name).toStdWString();

        // 1. 尝试连接已有管道（带 BUSY 短暂等待重试）
        HANDLE probe = INVALID_HANDLE_VALUE;
        for (int retry = 0; retry < 3; ++retry) {
            probe = CreateFileW(
                m_pipePath.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );
            if (probe != INVALID_HANDLE_VALUE)
                break;

            if (GetLastError() == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(m_pipePath.c_str(), 100);
            } else {
                break;
            }
        }

        if (probe != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const char msg[] = "show\n";
            WriteFile(probe, msg, static_cast<DWORD>(sizeof(msg) - 1), &written, nullptr);
            FlushFileBuffers(probe);
            CloseHandle(probe);
            m_running = true;
            return false;
        }

        // 2. 创建管道（设置安全属性：NULL DACL，允许提权与非提权进程互通；
        //    加 FILE_FLAG_FIRST_PIPE_INSTANCE 保证并发竞争时必定只有一个实例成功）
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;

        m_pipe = CreateNamedPipeW(
            m_pipePath.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            512,
            512,
            1000,
            &sa
        );

        if (m_pipe == INVALID_HANDLE_VALUE) {
            const DWORD errCode = GetLastError();
            // 若被同时启动的另一个进程抢先创建（ERROR_ACCESS_DENIED），说明已有实例
            if (errCode == ERROR_ACCESS_DENIED) {
                HANDLE probe2 = CreateFileW(m_pipePath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (probe2 != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    const char msg[] = "show\n";
                    WriteFile(probe2, msg, static_cast<DWORD>(sizeof(msg) - 1), &written, nullptr);
                    FlushFileBuffers(probe2);
                    CloseHandle(probe2);
                }
            }
            m_error = QString::asprintf("CreateNamedPipeW failed: %lu", errCode);
            if (err) *err = m_error;
            m_running = true;
            return false;
        }

        m_stop = false;

        // 3. 启动后台线程监听客户端连接
        m_worker = std::thread([this]() {
            while (!m_stop.load()) {
                BOOL connected = ConnectNamedPipe(m_pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
                if (m_stop.load())
                    break;

                if (connected) {
                    char buf[64] = { 0 };
                    DWORD bytesRead = 0;
                    if (ReadFile(m_pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                        std::string_view msg(buf, bytesRead);
                        if (msg.find("show") != std::string_view::npos) {
                            QMetaObject::invokeMethod(QCoreApplication::instance(), [this]() {
                                if (m_onActivate)
                                    m_onActivate();
                            }, Qt::QueuedConnection);
                        }
                    }
                    DisconnectNamedPipe(m_pipe);
                } else {
                    if (m_stop.load())
                        break;
                    Sleep(50);
                }
            }
        });

        return true;
#else
        return true;
#endif
    }

    bool alreadyRunning() const { return m_running; }
    const QString& errorString() const { return m_error; }

    void onActivate(ActivateCallback cb) { m_onActivate = std::move(cb); }

private:
    void stop() {
#ifdef Q_OS_WIN
        m_stop = true;

        // 关键防护：只有真正作为服务端创建了管道的实例，才需要唤醒并回收工作线程与句柄
        // 客户端实例（发现已有进程后退出者）m_pipe == INVALID_HANDLE_VALUE，绝不执行唤醒
        if (m_pipe != INVALID_HANDLE_VALUE) {
            if (!m_pipePath.empty()) {
                // 连接一次以唤醒阻塞在 ConnectNamedPipe 的工作线程
                HANDLE wake = CreateFileW(
                    m_pipePath.c_str(),
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
            }

            // 必须先等待工作线程彻底退出，再关闭管道句柄，防止竞态崩溃
            if (m_worker.joinable()) {
                m_worker.join();
            }

            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
#endif
    }

    ActivateCallback m_onActivate;
    bool m_running = false;
    QString m_error;

#ifdef Q_OS_WIN
    std::wstring m_pipePath;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::atomic<bool> m_stop{ false };
    std::thread m_worker;
#endif
};

} // namespace ts
