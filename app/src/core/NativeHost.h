#pragma once
#include <QString>
#include <QtGlobal>
#include <memory>

namespace ts {

// 浏览器扩展 → 本机程序 的归属数据通道（二期）。
//
// 为什么要绕一层：Chrome/Edge 的 Native Messaging host 必须是**用 stdio 通信的独立
// 可执行文件**，而 TrafficScope 主程序是 GUI 进程、且已经在跑事件循环，不能兼任。
// 于是拆成两段：
//   浏览器扩展 ──(stdio, 4字节长度前缀 + JSON)──▶ TrafficScopeHost.exe ──(命名管道)──▶ 主实例
//
// 单向、本机、不外发：扩展只回传"资源域名/IP → 站点域名"（**绝不回传 URL**），
// 字节数仍由抓包统计，两者互补。
class SiteBridge {
public:
    static SiteBridge& instance();
    ~SiteBridge();

    // 主实例启动时调用一次；失败不影响主流程（只是扩展归属这条链路不可用）
    bool listen(QString* err = nullptr);

    // 给设置页显示状态用。
    // lastFeedMs：最近一次**真的吃到归属条目**的时刻（0 = 从未）；
    // lastBatchItems：那一条消息里的条数；totalItems：本次运行累计条数。
    // 记条数而不是批次数：批次大小取决于扩展的合批窗口，对用户没意义。
    qint64 lastFeedMs() const;
    int lastBatchItems() const;
    quint64 totalItems() const;

private:
    SiteBridge();

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    qint64 m_lastFeedMs = 0;
    int m_lastBatchItems = 0;
    quint64 m_totalItems = 0;
};

// TrafficScopeHost.exe 的入口：从 stdin 读 native messaging 帧，转发给主实例。
// 浏览器断开（stdin 关闭）时返回 0。
int runNativeHost();

}  // namespace ts
