# TrafficScope Site Helper（浏览器扩展，可选）

给 TrafficScope 补一条它自己拿不到的信息，这个请求属于哪个网站。

抓包只能看到远端 IP 和 TLS 的 SNI，看不出某个 CDN 域名是哪个页面加载的。扩展站在浏览器里，知道请求由谁发起，能把 CDN 域名和裸 IP 归到具体站点上。字节数还是抓包算的，扩展只补归属关系，两边互补。

支持 Chrome 与 Edge，Manifest V3，当前版本 1.0.0。不装也能用，只是网站维度会粗一些，主程序按注册域名和 DNS CNAME 链自己推。

## 隐私边界

写死在代码注释里，不接受顺手加的改动。

- 只回传 hostname，`img.example.com` 这种。完整 URL、path、query、fragment 一律不读、不存、不发
- 不写本地存储，不发网络请求，不上报
- 通信只在本机进程之间，`chrome.runtime.connectNative` 连到 `TrafficScopeHost.exe`

这个扩展一旦开始回传完整 URL，它的性质就从流量统计变成记录浏览历史，那是另一回事，不在本项目的目标里。

## host 进程什么时候存在

平时没有 host 进程，只有主程序在跑并且你正在浏览网页时，才会短暂出现一个。

机制是运行期动态注册，实现见 `app/src/core/NativeHostReg.h`。

| 时刻 | 行为 |
|---|---|
| TrafficScope.exe 启动，设置里开着「启用浏览器扩展归属」 | 写 HKCU 键和 `%LOCALAPPDATA%\TrafficScope\native-host\com.trafficscope.site.json` |
| TrafficScope.exe 退出 | 删掉上面那两样 |
| 主程序没运行 | 注册表里没有登记，扩展 `connectNative()` 直接失败，浏览器不会拉起任何进程 |
| 主程序在跑、扩展攒到待发数据 | 扩展才 `connect()`，浏览器拉起 `TrafficScopeHost.exe`，转发完继续待命 |
| 连续 60 秒没有新数据 | 扩展主动 `disconnect()`，host 进程退出 |

所以不浏览网页就没有 host 进程，程序没开连失败都不会产生进程。注册表里不再有长期存在的登记项，崩溃残留会在下次启动时清掉，卸载时也会清。

## 安装

面向用户的操作步骤写在 `浏览器扩展安装步骤.txt` 里，安装包会把它放在安装目录的 `extension\` 下，程序里在「设置 → 统计」点「查看安装步骤」也能直接打开。要点如下。

1. 打开 `chrome://extensions`，Edge 是 `edge://extensions`
2. 打开右上角的开发者模式
3. 点「加载已解压的扩展程序」，选中本目录
4. 确认卡片上的 ID 是 `phdnbcbpkhpbodccdkncbpkfdlmakmcf`。`manifest.json` 里的 `key` 把这个 ID 钉死了，换目录、换机器都不会变，Native Messaging 的白名单依赖它
5. 装完不要移动、不要删除这个目录，否则扩展会失效

浏览器每次启动会弹一次「请停用开发者模式的扩展程序」，直接关掉就行，不影响使用。

从旧版本升级过来的，改完 `manifest.json` 后要在扩展卡片上点一次刷新按钮。

## Native Messaging host 注册

正常情况下什么都不用做，主程序运行时自己登记、退出时自己注销，上一节写了。

`install-host.ps1` 只在两种场景还用得上。

- 机器级（多用户）安装。加 `-System` 写 `HKLM`，需要提权终端
- 手工排查。用 `-ExePath "D:\dev\TrafficScopeHost.exe"` 指定 host 程序的真实位置

```powershell
powershell -ExecutionPolicy Bypass -File install-host.ps1
```

它做的事是生成 host 清单到 `%LOCALAPPDATA%\TrafficScope\native-host\`，并在 `HKCU` 下注册 Chrome 与 Edge 的 Native Messaging 键，加了 `-System` 就写 `HKLM`。手工注册也行，把 `host/com.trafficscope.site.json` 里的 `path` 改成实际的 `TrafficScopeHost.exe` 全路径，再 `reg add` 到对应键。

清单文件必须放在本地磁盘，不能是网络盘，而且不能带 BOM。

## 确认它在工作

主程序的「设置 → 统计」页有「启用浏览器扩展归属」开关和一行状态。

- 开关关掉时显示「已关闭（不登记，也不会有后台进程）」
- 开关开着但还没收到数据，显示「未连接（未收到扩展数据）」
- 收到数据后显示成「刚刚收到 X 条数据，共计收到 Q 条数据」，X 是距最近一次收到数据的时长，每次刷新实算，Q 是本次运行累计。打开几个网页就能看到它动

## 排错

| 现象 | 原因 |
|---|---|
| 状态显示「已关闭」 | 设置里把「启用浏览器扩展归属」关掉了 |
| 状态一直「未连接」 | 主程序没运行，不登记时连不上是正常的；或者扩展没刷新加载；或者 `allowed_origins` 的 ID 不符 |
| 浏览器控制台报 `Specified native messaging host not found` | 主程序没在跑，属于正常现象。主程序在跑还报这个，看主程序启动日志里的 `native host register failed` |
| 报 `Access to the specified native messaging host is forbidden` | `allowed_origins` 里的扩展 ID 与实际不符，说明 `key` 被改动过 |
| 数据偶尔断一下又恢复 | 空闲 60 秒后主动断开，设计如此，有网页流量时会自动重连 |
| 进程列表里始终有 `TrafficScopeHost.exe` | 说明还在浏览网页，有数据在发。停一分钟不动就会退出 |

## 已知限制

- Firefox 不支持。普通版只认签名扩展，`about:debugging` 的临时载入重启就失效
- 只覆盖浏览器流量。微信、云盘、游戏、系统更新没有网站归属，只在域名维度里出现
- 没装扩展时网站维度仍然有数据，主程序按注册域名和 DNS CNAME 链推断，粒度比扩展给的粗
