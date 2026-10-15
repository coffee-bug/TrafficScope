#include "FullScreenDetect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>   // SHQueryUserNotificationState

#include <cwchar>       // _wcsicmp

namespace ts {
namespace {

// 窗口是否完整盖住它所在的显示器。
// 容差几像素：有的程序会故意把窗口做得比屏幕大/小一点，严格相等会漏判。
bool coversWholeMonitor(HWND hwnd)
{
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!mon)
        return false;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return false;

    RECT r;
    if (!GetWindowRect(hwnd, &r))
        return false;

    const int tol = 4;
    return r.left   <= mi.rcMonitor.left   + tol
        && r.top    <= mi.rcMonitor.top    + tol
        && r.right  >= mi.rcMonitor.right  - tol
        && r.bottom >= mi.rcMonitor.bottom - tol;
}

// 游戏 / 演示模式由系统直接给出，不依赖窗口矩形。
// 它比"取一次窗口矩形"重一点，所以限制成 1s 一次；**前台窗口换了就立即查**
// （进游戏几乎都是切窗口，这样不会因为节流多等一拍）。
bool d3dFullscreen(HWND hwnd)
{
    constexpr DWORD kQueryIntervalMs = 1000;

    static HWND lastHwnd = nullptr;
    static DWORD lastTick = 0;

    const DWORD now = GetTickCount();
    const bool hwndChanged = (hwnd != lastHwnd);
    if (!hwndChanged && lastTick != 0 && (now - lastTick) < kQueryIntervalMs)
        return false;   // 无符号减法天然处理 GetTickCount 的 49 天回绕

    lastHwnd = hwnd;
    lastTick = now;

    QUERY_USER_NOTIFICATION_STATE st = QUNS_ACCEPTS_NOTIFICATIONS;
    return SHQueryUserNotificationState(&st) == S_OK
        && (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE);
}

// 桌面 / 任务栏这类外壳窗口的矩形本来就铺满屏幕，不能算全屏
bool isShellWindow(HWND hwnd)
{
    wchar_t cls[256] = {0};
    if (!GetClassNameW(hwnd, cls, 256))
        return false;
    return _wcsicmp(cls, L"Progman") == 0
        || _wcsicmp(cls, L"WorkerW") == 0
        || _wcsicmp(cls, L"Shell_TrayWnd") == 0
        || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

}  // namespace

bool isFullscreenAppActive()
{
    // ① 前台窗口盖满它所在的显示器（几次 Win32 取值，可以每拍都跑）
    HWND hwnd = GetForegroundWindow();
    if (hwnd && IsWindowVisible(hwnd) && !isShellWindow(hwnd)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()   // 自己的窗口不算（详情/设置弹窗最大化）
            && coversWholeMonitor(hwnd))
            return true;
    }

    // ② 游戏 / 演示模式兜底：独占全屏时窗口矩形未必取得到，但系统一定知道
    return d3dFullscreen(hwnd);
}

}  // namespace ts
