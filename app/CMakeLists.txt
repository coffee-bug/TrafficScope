cmake_minimum_required(VERSION 3.16)
project(TrafficScope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 源码是 UTF-8（无 BOM）：显式告诉 MSVC 按 UTF-8 解析（含执行字符集）。
# 否则 QStringLiteral("↓ ...")、"ⓘ"、中文提示这些字面量可能被按本地代码页解释，
# 编进 exe 就是乱码（也顺带消掉 C4819 警告）。
if (MSVC)
    add_compile_options(/utf-8)
endif()

# ---- Qt ----
find_package(Qt6 COMPONENTS Widgets QUIET)
if (Qt6_FOUND)
    set(QT_TARGET Qt6::Widgets)
    set(QT_CORE_TARGET Qt6::Core)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets)
    set(QT_TARGET Qt5::Widgets)
    set(QT_CORE_TARGET Qt5::Core)
endif()

add_executable(TrafficScope WIN32
    # 程序图标（exe 文件本身那枚，见 resources/app.rc；窗口图标由 ui/AppIcon.h 现画）
    resources/app.rc
    src/main.cpp
    src/core/EtwPacketSource.cpp
    src/core/PacketParser.cpp
    src/core/DnsSniffer.cpp
    src/core/TlsSniParser.cpp
    src/core/SiteName.cpp
    src/core/SiteResolver.cpp
    src/core/NativeHost.cpp
    src/core/NativeHostReg.cpp
    src/core/TrafficStore.cpp
    src/core/NetInterfacePicker.cpp
    src/core/ProcTraffic.cpp
    src/core/StatsDb.cpp
    src/core/StatsRecorder.cpp
    src/core/EtwSniffer.cpp
    src/core/FullScreenDetect.cpp
    src/core/AutoStart.cpp
    src/core/PacketSource.cpp
    src/ui/FloatWidget.cpp
    src/ui/TopPanel.cpp
    src/ui/DetailDialog.cpp
    src/ui/SettingsDialog.cpp
    src/ui/RangePopup.cpp
    # 纯头文件，但因为里面有带 Q_OBJECT 的 ThemeNotifier，必须列进目标让 AUTOMOC 处理
    src/ui/UiTheme.h
)

# ---- TrafficScopeHost.exe：浏览器扩展的 Native Messaging host（二期，可选链路）----
# 独立的小 exe：只做「从 stdin 读扩展消息 → 转发给主实例」，不碰抓包/统计/UI。
# WIN32 子系统（无控制台窗口）：Chrome 拉起 host 时弹黑框很难看；Windows 下 GUI
# 进程照样能读父进程给的 stdio 句柄。
add_executable(TrafficScopeHost WIN32
    src/host_main.cpp
    src/core/NativeHost.cpp
    src/core/SiteName.cpp
    src/core/SiteResolver.cpp
    src/core/DnsSniffer.cpp
)
target_include_directories(TrafficScopeHost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(TrafficScopeHost PRIVATE ${QT_CORE_TARGET})

target_include_directories(TrafficScope PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(TrafficScope PRIVATE
    ${QT_TARGET} winsqlite3 ws2_32 iphlpapi version shell32 tdh advapi32)

# 抓包必须管理员权限：把 requestedExecutionLevel=requireAdministrator 嵌进 exe 清单，
# 双击启动即提权（弹一次 UAC）。
# 开机自启动不走注册表 Run（那样每次登录都会弹 UAC），而是由 core/AutoStart 建一个
# ONLOGON + 最高权限的计划任务：任务创建时就拿到了提权令牌，之后登录时静默拉起。
if (MSVC)
    target_link_options(TrafficScope PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()
