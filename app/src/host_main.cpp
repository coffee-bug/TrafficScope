// TrafficScopeHost.exe —— 浏览器扩展的 Native Messaging host。
//
// 它被 Chrome / Edge 按需拉起，从 stdin 读扩展送来的归属数据，转发给主实例后退出。
// 不做任何统计、不碰抓包、不写数据库 —— 只是一条本机管道。
//
// 用 WIN32 子系统（无控制台窗口）：Chrome 拉起 host 时若弹出黑框会非常刺眼。
// Windows 下 GUI 子系统进程照样能拿到父进程给的 stdin/stdout 句柄，
// 用 GetStdHandle + ReadFile 直接读即可（见 core/NativeHost.cpp 的 readFully）。
#include "core/NativeHost.h"

#include <QCoreApplication>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    return ts::runNativeHost();
}
