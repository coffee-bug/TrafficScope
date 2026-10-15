// TrafficScope Site Helper — 只告诉本机程序"这个请求属于哪个网站"。
//
// 隐私红线（改这个文件前先看）：**只发域名，不发 URL**。
//   - 站点名取发起页的 hostname（initiator），资源名取资源的 hostname
//   - URL 的 path / query / fragment 一律不读、不存、不发
//   - 不写任何本地存储、不做任何网络请求
// 这个扩展一旦开始回传完整 URL，性质就从"流量统计"变成"记录浏览历史"，
// 那是另一回事，不在本项目的目标内。
//
// 为什么需要它：抓包只能看到 IP 与 SNI，看不到"这个 CDN 域名属于哪个页面"。
// 字节数仍然由抓包提供，这里只补归属关系 —— 两者是互补，不是替代。
//
// host 进程的生命周期（2026-10-12 改）：**按需连接 + 空闲断开**。
//   · 不再"worker 一醒来就连"、也没有常驻心跳；
//   · 只有攒到待发数据才 connectNative()；
//   · 空闲 IDLE_MS 后主动 port.disconnect() → 浏览器随之关掉 host 的 stdio，
//     TrafficScopeHost.exe 进程退出。平时不浏览网页 = 一个 host 进程都不该有。
//   · 主程序侧只在**运行期间**才登记 native host（见 app/src/core/NativeHostReg.h），
//     所以主程序没跑时 connectNative() 会直接失败且**不会产生任何进程**，
//     失败重试因此是零成本的。

const HOST_NAME = 'com.trafficscope.site';
const FLUSH_MS = 500;      // 合并窗口：一个页面会瞬间打出几十个请求
const MAX_BATCH = 300;     // 单批上限，防止一次性堆积过多
const RECONNECT_MS = 5000; // host 断开（主程序退出等）后的重试间隔
const IDLE_MS = 60000;     // 空闲这么久就主动断开，让 host 进程退出

let port = null;
let reconnectTimer = null;
let idleTimer = null;
let closing = false;       // true = 我们自己主动断开，onDisconnect 里不要再重连
// 去重用的 key：同一组合反复出现只发一次（主程序侧有 TTL 缓存，重复发没意义）
const pending = new Map();

function hostnameOf(url) {
  try {
    return new URL(url).hostname;
  } catch (e) {
    return '';
  }
}

// 站点归属：优先用 initiator（发起这个请求的页面 origin）。
// 拿不到 initiator 时退回资源自己的 hostname —— 主程序会再按注册域名兜底。
function collectInitiatorHost(details) {
  if (details.initiator && details.initiator !== 'null') {
    const h = hostnameOf(details.initiator);
    if (h) return h;
  }
  // 顶层导航（在地址栏敲网址）没有 initiator，此时资源自己就是站点
  if (details.frameId === 0 && details.type === 'main_frame') {
    return hostnameOf(details.url);
  }
  return '';
}

function onRequest(details) {
  const resourceHost = hostnameOf(details.url);
  if (!resourceHost) return;

  const siteHost = collectInitiatorHost(details);
  if (!siteHost) return;

  const ip = details.ip || '';
  const key = siteHost + '|' + resourceHost + '|' + ip;
  if (pending.has(key)) return;
  pending.set(key, { site: siteHost, domain: resourceHost, ip: ip });

  scheduleFlush();
}

let flushTimer = null;
function scheduleFlush() {
  if (flushTimer) return;
  flushTimer = setTimeout(() => {
    flushTimer = null;
    flush();
  }, FLUSH_MS);
}

function flush() {
  if (pending.size === 0) return;
  if (!port) {
    connect();
    return;   // 连上之后会再 flush 一次
  }

  const items = [];
  for (const v of pending.values()) {
    items.push(v);
    if (items.length >= MAX_BATCH) break;
  }
  // 先清空再发：发送失败也不至于无限重试同一批
  pending.clear();

  try {
    port.postMessage({ type: 'site', v: 1, items: items });
    scheduleIdleDisconnect();   // 有流量就把"空闲断开"往后推
  } catch (e) {
    port = null;
    scheduleReconnect();
  }
}

function connect() {
  if (port) return;
  try {
    port = chrome.runtime.connectNative(HOST_NAME);
  } catch (e) {
    // 主程序没跑时最常见的就是这里抛 "native messaging host not found"：
    // 注册表里没有登记，浏览器不会 spawn 进程，所以重试不花代价。
    port = null;
    scheduleReconnect();
    return;
  }
  port.onDisconnect.addListener(() => {
    // 断开有三种情况：主程序退出（host 进程结束） / worker 被浏览器回收 /
    // 我们自己空闲断开。前两种**无条件重连**（早期版本只在 pending 非空时才连，
    // 队列刚好空着就永远不连了，表现为"用过一阵之后再也连不上"）；第三种按设计不连。
    port = null;
    if (closing) {
      closing = false;
      return;
    }
    scheduleReconnect();
  });
  if (pending.size > 0) flush();
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect();
  }, RECONNECT_MS);
}

// 空闲断开：让 host 进程退出，避免"程序开着就常驻一个后台进程"。
// 下一次有待发数据时 flush() → connect() 会重新拉起（此时主程序在跑、登记存在）。
function scheduleIdleDisconnect() {
  if (idleTimer) clearTimeout(idleTimer);
  idleTimer = setTimeout(() => {
    idleTimer = null;
    if (!port) return;
    closing = true;
    try {
      port.disconnect();
    } catch (e) {
      closing = false;
      port = null;
    }
  }, IDLE_MS);
}

chrome.webRequest.onResponseStarted.addListener(onRequest, { urls: ['<all_urls>'] });

// 注意：这里**故意不**在 worker 启动时 connect()，也没有常驻 alarms 心跳。
// 连接由数据驱动（onRequest → flush → connect），断开由 onDisconnect 驱动重连。
