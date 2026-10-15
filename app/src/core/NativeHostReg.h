#pragma once
// 浏览器扩展 Native Messaging host 的**运行期注册**（只在主程序里用）。
//
// 为什么是运行期而不是安装期：浏览器只在「注册表里登记过该 host」时才会去 spawn
// TrafficScopeHost.exe。把登记与主程序的生命周期绑起来之后，
// **「登记存在」本身就等于「主程序在跑，扩展该干活了」**：
//   主程序启动（设置里开着）→ 写 HKCU 键 + manifest 文件
//   主程序退出              → 删掉
// 于是主程序没跑时，扩展的 connectNative() 会直接失败，**一个进程都不会产生**
// —— 这正是我们要的"程序没开就不该有 host 进程"。
//
// 范围与口径：
//   · 只写 HKCU（当前用户），Chrome 与 Edge 各一套键路径；注销时连 HKLM 的遗留
//     一起清（谁用 install-host.ps1 -System 装过机器级也能收干净）。
//   · host 名 / 扩展 ID 与 extension/manifest.json 一一对应，改那边要同步改这里。
//   · 幂等：重复注册 = 覆盖写，重复注销 = 无副作用。
//   · 主程序崩溃不会走到注销 → 下次启动先无条件清一次（见 main.cpp 的自愈）。

#include <QString>

namespace ts {

// 写 manifest 文件 + Chrome/Edge 的 HKCU 注册表项。
// 返回 false 时 err 里是原因（找不到 host exe / 目录写不了 / 注册表写失败）。
// 失败不影响主流程：扩展那条链路不可用而已，「网站」维度就没有数据。
bool browserHostRegister(QString* err = nullptr);

// 删掉 Chrome/Edge 的 HKCU 与 HKLM 注册项 + manifest 文件（含空目录）。
// 键不存在时静默返回，可放心在启动时无条件调一次。
void browserHostUnregister();

}  // namespace ts
