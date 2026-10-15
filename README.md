# TrafficScope

Windows 桌面流量浮框。无边框小窗常驻桌面，实时显示本机上传与下载速率，流量按应用、域名、网站三个维度归因，知道是谁在跑、跑到哪儿去了。

- 纯用户态实现，基于系统原生 ETW 抓包，不装内核驱动，不装证书，不代理流量
- 数据全留本机，统计落 SQLite，配置落 ini，不联网上报
- 零第三方运行库与抓包驱动依赖，纯净绿色
- 深色浮框，托盘常驻
- 支持 Windows 10 与 11，64 位
- 本项目全程使用 AI 开发完成

## 下载安装

到本仓库的发行版页面下载 `TrafficScope-Setup-<版本>-x64.exe`，双击安装即可直接使用，不需要事先安装任何第三方抓包驱动。

程序需要管理员权限（系统级 ETW 抓包和连接归因要求提权），exe 内已内嵌提权清单，启动时允许一次 UAC 即可，无需手动右键「以管理员身份运行」。

## 功能概览

- **浮框** 单击展开实时榜，分上传 Top 与下载 Top；拖到屏幕边缘自动吸附收起；鼠标能拖着走；有程序全屏时自动隐藏，退出全屏恢复；托盘常驻
- **详情弹窗** 应用、域名、网站、历史累计四个页签，都能排序、搜索、导出 CSV。应用页可以定位 exe、展开看 PID 明细、结束进程；历史累计页支持自选时间区间与趋势柱状图
- **设置** 显示、统计、历史记录、网卡、关于五页
- **归因** 应用维度靠内核 TCPIP 事件加连接表快照四层兜底；域名维度嗅探 DNS 应答与 TLS SNI；网站维度按扩展归属、DNS CNAME 链、注册域名三级回退

功能细节、归因原理、数据落盘位置、已知限制都在 `app/README.md`。

## 仓库结构

```
TrafficScope
├── README.md
├── LICENSE                  木兰宽松许可证第 2 版
├── app/                     C++ 主程序
│   ├── README.md            软件说明
│   ├── CMakeLists.txt       两个 target，TrafficScope 与 TrafficScopeHost
│   ├── src/                 core 抓包归因存储与系统集成，ui 浮框、弹窗、图表
│   └── resources/           图标与 exe 资源
└── extension/               浏览器扩展，可选，用来补精确的网站归属
```

## 从源码构建

构建所需环境与工具。

| 依赖 | 说明 |
|---|---|
| Windows 10（1809+）或 11，64 位 | 目标平台 |
| Visual Studio 2022（MSVC） | C++17，Windows SDK 26100 上验证过 |
| CMake 3.16 以上 | 构建工具 |
| Qt 6（Widgets） | 只要 Widgets。没找到 Qt6 时回退 Qt 5.15 的 Widgets。SQLite 用系统自带的 `winsqlite3.dll`，抓包直接调系统自带 ETW 跟踪，无需额外库 |

```powershell
cd app
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build --config Release

# 把 Qt 运行库部署到 exe 旁边，换或加 Qt 模块后必须重跑
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations `
  --skip-plugin-types generic,tls,networkinformation,iconengines,sqldrivers `
  build\Release\TrafficScope.exe
```

编出来两个 exe，`build\Release\TrafficScope.exe` 是主程序，`TrafficScopeHost.exe` 是浏览器扩展的 Native Messaging 宿主。运行时以管理员身份启动。

## 数据与隐私

统计与配置都在 `%LOCALAPPDATA%\TrafficScope`，不写安装目录，升级重装都不会被清掉，卸载时可以选择保留。程序不联网，不上报，唯一一处注册表写入是浏览器扩展的 Native Messaging 登记项，只在运行期间存在，退出就删，关掉「启用浏览器扩展归属」可以完全不做。

浏览器扩展只回传 hostname，不读完整 URL、路径和查询串，不写本地存储，不发网络请求。

落盘文件与键位的清单在 `app/README.md`。

## 浏览器扩展

`extension/` 下是一个可选的 Chrome 与 Edge 扩展。抓包拿不到「这个 CDN 域名是哪个页面加载的」这类信息，扩展在浏览器里能看到，把归属回传给本机的主程序，网站维度会更准。不装也不影响其它功能，主程序自己按注册域名和 DNS CNAME 链推断。

安装步骤和排错见 `extension/README.md`。

## 第三方组件与许可

本项目代码按木兰宽松许可证第 2 版发布，全文见 [LICENSE](LICENSE)。

第三方组件方面，Qt 以动态链接方式使用，遵循 LGPL。源码本身不依赖任何第三方驱动与 C++ 库，数据包捕获（NDIS ETW）、连接归因（TCPIP ETW）、TDH、SQLite（winsqlite3）都直接调用 Windows 系统原生 API。
