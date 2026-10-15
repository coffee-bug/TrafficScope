#include "ProcTraffic.h"

#include "EtwSniffer.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QMutexLocker>
#include <QReadLocker>
#include <QSet>
#include <QWriteLocker>
#include <string>
#include <vector>

namespace {

const QString kUnknown = QStringLiteral("未知进程");   // 未知进程
const QString kSysDesc = QStringLiteral("系统进程");   // 系统进程
// 程序类别（与 kSysDesc 是两回事：那个是 pid=4 的"描述"）
const QString kCatSystem = QStringLiteral("系统程序");   // 系统程序
const QString kCatThird = QStringLiteral("第三方程序");   // 第三方程序
// pid=0：查不出归属的流量（不是一个进程），单独命名，避免被误读成"隐藏进程"
const QString kUnattributed = QStringLiteral("未知流量");   // 未知流量
// 旧版本用的名字：改名不回溯改库，历史库里可能还存着，过滤时要一并认
const QString kUnattributedLegacy = QStringLiteral("未归因流量");   // 未归因流量

// 端口级记忆的有效窗口，TCP / UDP 刻意分开设：
//  - TCP 取长（120s）：TIME_WAIT 会持续 30~120s，而这期间 Windows 不会把同一
//    四元组分配给新连接，所以"记住原主人"是安全的，可把 TIME_WAIT 里的残留包
//    归回原进程（实测这是 pid=0 的最大单一来源，占样本的 45%）；
//  - UDP 取短（15s）：UDP 没有 TIME_WAIT，端口随时可能被别的进程接管，
//    窗口长了会把新进程的流量错记到旧进程头上。
const qint64 kPortMemoTcpMs = 120000;
const qint64 kPortMemoUdpMs = 15000;

// 把本拍的连接记进端口级记忆：(本机地址, 本机端口) -> PID。
// pid==0 的行（TIME_WAIT 等无主连接）跳过，避免把"无主"固化进记忆。
void rememberPorts(QHash<TsConnKey, TsPortMemo>& memo,
                   const QHash<TsConnKey, quint64>& table, qint64 now, qint64 windowMs)
{
    for (auto it = table.constBegin(); it != table.constEnd(); ++it) {
        if (it.value() == 0)
            continue;
        TsConnKey k;
        k.localAddr = it.key().localAddr;
        k.localPort = it.key().localPort;
        TsPortMemo& m = memo[k];
        m.pid = it.value();
        m.ms = now;
    }
    for (auto it = memo.begin(); it != memo.end();) {
        if (now - it->ms > windowMs)
            it = memo.erase(it);
        else
            ++it;
    }
}

// 读取 exe 版本资源里的 FileDescription（资源管理器「详细信息」中的文件描述）。
// 需链接 Version.lib（见 CMakeLists）。失败返回空串，UI 侧退化为单行。
QString fileDescription(const QString& path)
{
    if (path.isEmpty())
        return QString();

    const std::wstring wpath = path.toStdWString();
    DWORD dummy = 0;
    const DWORD sz = GetFileVersionInfoSizeW(wpath.c_str(), &dummy);
    if (sz == 0)
        return QString();

    std::vector<char> buf(sz);
    if (!GetFileVersionInfoW(wpath.c_str(), 0, sz, buf.data()))
        return QString();

    // 版本资源的字符串按 语言+代码页 分组，先取可用分组列表
    struct LangCodePage { WORD lang; WORD codePage; };
    LangCodePage* trans = nullptr;
    UINT transLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID*>(&trans), &transLen)
        || transLen < sizeof(LangCodePage) || trans == nullptr) {
        return QString();
    }

    const int groups = int(transLen / sizeof(LangCodePage));
    for (int i = 0; i < groups; ++i) {
        const QString sub = QStringLiteral("\\StringFileInfo\\%1%2\\FileDescription")
                                .arg(uint(trans[i].lang), 4, 16, QLatin1Char('0'))
                                .arg(uint(trans[i].codePage), 4, 16, QLatin1Char('0'));
        const std::wstring wsub = sub.toStdWString();
        WCHAR* val = nullptr;
        UINT vlen = 0;
        if (!VerQueryValueW(buf.data(), const_cast<LPWSTR>(wsub.c_str()),
                            reinterpret_cast<LPVOID*>(&val), &vlen)
            || vlen == 0 || val == nullptr) {
            continue;
        }
        QString d = QString::fromWCharArray(val, int(vlen));
        const int nul = d.indexOf(QChar(0));   // 资源串以 NUL 结尾，多余内容截断
        if (nul >= 0)
            d.truncate(nul);
        d = d.trimmed();
        if (!d.isEmpty())
            return d;
    }
    return QString();
}

void resolveProcess(quint64 pid, QString& name, QString& path, QString& desc)
{
    if (pid == 0)
        return;   // 保持空，由 nameFor 统一归为"未知进程"
    if (pid == 4) {
        name = QStringLiteral("System");
        desc = kSysDesc;
        return;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return;
    WCHAR buf[MAX_PATH * 2] = {0};
    DWORD len = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        path = QString::fromWCharArray(buf, int(len));
        name = QFileInfo(path).fileName();
        desc = fileDescription(path);
    }
    CloseHandle(h);
}

} // namespace

ProcTraffic& ProcTraffic::instance()
{
    static ProcTraffic s;
    return s;
}

void ProcTraffic::refresh()
{
    QHash<TsConnKey, quint64> tcp;
    QHash<TsConnKey, quint64> udp;
    // pid -> 已建立（ESTABLISHED）的 TCP 连接数。
    // 口径：只数已建立的连接，监听中 / 正在握手 / 等待回收 的都不算
    //（用户明确要求"还未建立连接的不管"）。
    QHash<quint64, int> conn;
    QSet<quint64> pids;

    // IPv4 / IPv6 共用同一张表（key 已统一为 16 字节）
    auto addTcp = [&](const IpKey& la, uint16_t lp, const IpKey& ra, uint16_t rp,
                      quint64 pid, bool estab) {
        TsConnKey k;
        k.localAddr = la;
        k.localPort = lp;
        k.remoteAddr = ra;
        k.remotePort = rp;
        tcp.insert(k, pid);
        pids.insert(pid);
        if (estab)
            ++conn[pid];
    };
    auto addUdp = [&](const IpKey& la, uint16_t lp, quint64 pid) {
        TsConnKey k;
        k.localAddr = la;
        k.localPort = lp;
        udp.insert(k, pid);
        pids.insert(pid);
    };

    // ---- TCP 连接表（IPv4）----
    ULONG sz = 0;
    if (GetExtendedTcpTable(nullptr, &sz, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0)
            == ERROR_INSUFFICIENT_BUFFER
        && sz > 0) {
        std::vector<char> buf(sz);
        if (GetExtendedTcpTable(buf.data(), &sz, FALSE, AF_INET,
                                TCP_TABLE_OWNER_PID_ALL, 0)
            == NO_ERROR) {
            auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCPROW_OWNER_PID& r = t->table[i];
                addTcp(IpKey::fromV4(ntohl(r.dwLocalAddr)),
                       ntohs(uint16_t(r.dwLocalPort)),
                       IpKey::fromV4(ntohl(r.dwRemoteAddr)),
                       ntohs(uint16_t(r.dwRemotePort)),
                       r.dwOwningPid, r.dwState == MIB_TCP_STATE_ESTAB);
            }
        }
    }

    // ---- TCP 连接表（IPv6）----
    sz = 0;
    if (GetExtendedTcpTable(nullptr, &sz, FALSE, AF_INET6,
                            TCP_TABLE_OWNER_PID_ALL, 0)
            == ERROR_INSUFFICIENT_BUFFER
        && sz > 0) {
        std::vector<char> buf(sz);
        if (GetExtendedTcpTable(buf.data(), &sz, FALSE, AF_INET6,
                                TCP_TABLE_OWNER_PID_ALL, 0)
            == NO_ERROR) {
            auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCP6ROW_OWNER_PID& r = t->table[i];
                // 注意：IPv6 行结构用的是 ucLocalAddr / ucRemoteAddr / dwState(DWORD)
                addTcp(IpKey::fromV6(r.ucLocalAddr),
                       ntohs(uint16_t(r.dwLocalPort)),
                       IpKey::fromV6(r.ucRemoteAddr),
                       ntohs(uint16_t(r.dwRemotePort)),
                       r.dwOwningPid, r.dwState == MIB_TCP_STATE_ESTAB);
            }
        }
    }

    // ---- UDP 绑定表（无远端，remote 恒 0）----
    sz = 0;
    if (GetExtendedUdpTable(nullptr, &sz, FALSE, AF_INET,
                            UDP_TABLE_OWNER_PID, 0)
            == ERROR_INSUFFICIENT_BUFFER
        && sz > 0) {
        std::vector<char> buf(sz);
        if (GetExtendedUdpTable(buf.data(), &sz, FALSE, AF_INET,
                                UDP_TABLE_OWNER_PID, 0)
            == NO_ERROR) {
            auto* t = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_UDPROW_OWNER_PID& r = t->table[i];
                addUdp(IpKey::fromV4(ntohl(r.dwLocalAddr)),
                       ntohs(uint16_t(r.dwLocalPort)), r.dwOwningPid);
            }
        }
    }

    // ---- UDP 绑定表（IPv6）----
    sz = 0;
    if (GetExtendedUdpTable(nullptr, &sz, FALSE, AF_INET6,
                            UDP_TABLE_OWNER_PID, 0)
            == ERROR_INSUFFICIENT_BUFFER
        && sz > 0) {
        std::vector<char> buf(sz);
        if (GetExtendedUdpTable(buf.data(), &sz, FALSE, AF_INET6,
                                UDP_TABLE_OWNER_PID, 0)
            == NO_ERROR) {
            auto* t = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_UDP6ROW_OWNER_PID& r = t->table[i];
                addUdp(IpKey::fromV6(r.ucLocalAddr),
                       ntohs(uint16_t(r.dwLocalPort)), r.dwOwningPid);
            }
        }
    }

    // ---- 新 PID 补充进程名缓存（OpenProcess 放在锁外，避免阻塞抓包线程）----
    QList<quint64> missing;
    {
        QReadLocker l(&m_lock);
        for (quint64 pid : pids)
            if (!m_nameCache.contains(pid))
                missing.append(pid);
    }

    QHash<quint64, QString> newName;
    QHash<quint64, QString> newPath;
    QHash<quint64, QString> newDesc;
    for (quint64 pid : missing) {
        QString name, path, desc;
        resolveProcess(pid, name, path, desc);
        if (name.isEmpty())
            name = kUnknown;
        newName.insert(pid, name);
        if (!path.isEmpty())
            newPath.insert(pid, path);
        if (!desc.isEmpty())
            newDesc.insert(pid, desc);
    }

    {
        QWriteLocker l(&m_lock);
        // 端口记忆必须在 std::move 之前用原表更新
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        rememberPorts(m_tcpPortMemo, tcp, now, kPortMemoTcpMs);
        rememberPorts(m_udpPortMemo, udp, now, kPortMemoUdpMs);
        m_tcpMap = std::move(tcp);
        m_udpMap = std::move(udp);
        m_connCount = std::move(conn);
        for (auto it = newName.constBegin(); it != newName.constEnd(); ++it) {
            m_nameCache.insert(it.key(), it.value());
            const QString p = newPath.value(it.key());
            if (!p.isEmpty())
                m_pathCache.insert(it.key(), p);
            const QString d = newDesc.value(it.key());
            if (!d.isEmpty())
                m_descCache.insert(it.key(), d);
        }
    }
}

quint64 ProcTraffic::pidFor(bool tcp, const IpKey& localIp, uint16_t localPort,
                            const IpKey& remoteIp, uint16_t remotePort) const
{
    // 1) ETW 实时映射优先：连接建立/断开时由内核事件直接给出 PID，
    //    没有采样间隙，短命连接也能命中。
    const quint64 etwPid = EtwSniffer::instance().pidFor(tcp, localIp, localPort,
                                                        remoteIp, remotePort);
    if (etwPid != 0)
        return etwPid;

    // 2) 轮询快照兜底（300ms 刷新 + 端口级记忆）
    TsConnKey k;
    k.localAddr = localIp;
    k.localPort = localPort;
    k.remoteAddr = tcp ? remoteIp : IpKey();
    k.remotePort = tcp ? remotePort : 0;

    QReadLocker l(&m_lock);
    const QHash<TsConnKey, quint64>& m = tcp ? m_tcpMap : m_udpMap;

    auto it = m.constFind(k);
    if (it != m.constEnd())
        return *it;

    // 兜底 1：绑定在 0.0.0.0 / :: 的 socket（常见于 UDP 出站）
    if (!localIp.isZero()) {
        TsConnKey k2 = k;
        k2.localAddr = IpKey();
        it = m.constFind(k2);
        if (it != m.constEnd())
            return *it;
    }

    // 兜底 2：端口级短期记忆。短命连接的生命周期可能整个落在两次快照之间，
    // 精确表里已经没有它，但 (本机地址:端口) 的归属还在记忆窗口内。
    const QHash<TsConnKey, TsPortMemo>& memo = tcp ? m_tcpPortMemo : m_udpPortMemo;
    TsConnKey pk;
    pk.localAddr = localIp;
    pk.localPort = localPort;
    auto mit = memo.constFind(pk);
    if (mit != memo.constEnd())
        return mit->pid;
    if (!localIp.isZero()) {
        pk.localAddr = IpKey();
        mit = memo.constFind(pk);
        if (mit != memo.constEnd())
            return mit->pid;
    }

    // 仍未命中：记为未归因（诊断采样，容量有限且去重，抓包线程内开销极小）
    if (tcp)
        rememberMiss(k);
    return 0;
}

const QString& ProcTraffic::unattributedName()
{
    return kUnattributed;
}

bool ProcTraffic::isUnattributedName(const QString& name)
{
    return name == kUnattributed || name == kUnattributedLegacy;
}

QString ProcTraffic::nameFor(quint64 pid) const
{
    if (pid == 0)
        return kUnattributed;
    QReadLocker l(&m_lock);
    return m_nameCache.value(pid, kUnknown);
}

QString ProcTraffic::pathFor(quint64 pid) const
{
    if (pid == 0)
        return QString();
    QReadLocker l(&m_lock);
    return m_pathCache.value(pid);
}

QString ProcTraffic::categoryFor(quint64 pid) const
{
    if (pid == 0)
        return QString();                 // 未归因流量不是一个进程
    if (pid == 4)
        return kCatSystem;                // System
    const QString p = pathFor(pid);
    if (p.isEmpty())
        return kCatSystem;                // 路径都拿不到的多半是受保护的系统进程
    // Windows 目录（含 System32 / SysWOW64 / WinSxS）下视为系统程序
    const QString root = qEnvironmentVariable("SystemRoot");
    const QString native = QDir::toNativeSeparators(p);
    if (!root.isEmpty()) {
        const QString prefix = QDir::toNativeSeparators(root) + QLatin1Char('\\');
        if (native.startsWith(prefix, Qt::CaseInsensitive))
            return kCatSystem;
    }
    return kCatThird;                     // 第三方程序
}

QString ProcTraffic::descFor(quint64 pid) const
{
    if (pid == 0)
        return QString();
    QReadLocker l(&m_lock);
    return m_descCache.value(pid);
}

int ProcTraffic::connCountFor(quint64 pid) const
{
    if (pid == 0)
        return 0;
    QReadLocker l(&m_lock);
    return m_connCount.value(pid, 0);
}

// ---- 连接列表 / 断开连接 / 结束进程（详情弹窗用）----

QVector<quint64> ProcTraffic::pidsForName(const QString& name) const
{
    QVector<quint64> out;
    // pid=0 不是进程（归因失败的流量），没有可执行体
    if (name.isEmpty() || name == unattributedName())
        return out;
    if (name == QLatin1String("System")) {
        out.append(4);
        return out;
    }
    QReadLocker l(&m_lock);
    for (auto it = m_nameCache.constBegin(); it != m_nameCache.constEnd(); ++it) {
        if (it.value() == name)
            out.append(it.key());
    }
    return out;
}

bool ProcTraffic::terminateProcess(quint64 pid)
{
    if (pid == 0)
        return false;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (!h)
        return false;
    const BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok != FALSE;
}

// ---- 诊断：未归因 TCP 包在连接表里的状态分布 ----

QString ProcTraffic::tcpStateText(quint32 s)
{
    switch (s) {
    case MIB_TCP_STATE_CLOSED:     return QStringLiteral("CLOSED");
    case MIB_TCP_STATE_LISTEN:     return QStringLiteral("LISTEN");
    case MIB_TCP_STATE_SYN_SENT:   return QStringLiteral("SYN_SENT");
    case MIB_TCP_STATE_SYN_RCVD:   return QStringLiteral("SYN_RCVD");
    case MIB_TCP_STATE_ESTAB:      return QStringLiteral("ESTAB");
    case MIB_TCP_STATE_FIN_WAIT1:  return QStringLiteral("FIN_WAIT1");
    case MIB_TCP_STATE_FIN_WAIT2:  return QStringLiteral("FIN_WAIT2");
    case MIB_TCP_STATE_CLOSE_WAIT: return QStringLiteral("CLOSE_WAIT");
    case MIB_TCP_STATE_CLOSING:    return QStringLiteral("CLOSING");
    case MIB_TCP_STATE_LAST_ACK:   return QStringLiteral("LAST_ACK");
    case MIB_TCP_STATE_TIME_WAIT:  return QStringLiteral("TIME_WAIT");
    case MIB_TCP_STATE_DELETE_TCB: return QStringLiteral("DELETE_TCB");
    default:                       return QStringLiteral("?");
    }
}

void ProcTraffic::rememberMiss(const TsConnKey& k) const
{
    QMutexLocker l(&m_missLock);
    constexpr int kMaxMissSamples = 64;   // 诊断样本上限：只留最近若干条，防无限增长
    if (m_missSamples.size() >= kMaxMissSamples)
        return;
    if (m_missSamples.contains(k))
        return;
    m_missSamples.append(k);
}

QString ProcTraffic::missStateReport() const
{
    QVector<TsConnKey> samples;
    {
        QMutexLocker l(&m_missLock);
        samples = m_missSamples;
    }
    if (samples.isEmpty())
        return QStringLiteral("(无样本)");

    // 现场读一次 TCP 连接表：四元组 -> 状态（只为诊断，频率很低）
    QHash<TsConnKey, quint32> states;
    ULONG sz = 0;
    if (GetExtendedTcpTable(nullptr, &sz, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
            == ERROR_INSUFFICIENT_BUFFER
        && sz > 0) {
        std::vector<char> buf(sz);
        if (GetExtendedTcpTable(buf.data(), &sz, FALSE, AF_INET,
                                TCP_TABLE_OWNER_PID_ALL, 0)
            == NO_ERROR) {
            auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCPROW_OWNER_PID& r = t->table[i];
                TsConnKey k;
                k.localAddr = IpKey::fromV4(ntohl(r.dwLocalAddr));
                k.localPort = ntohs(uint16_t(r.dwLocalPort));
                k.remoteAddr = IpKey::fromV4(ntohl(r.dwRemoteAddr));
                k.remotePort = ntohs(uint16_t(r.dwRemotePort));
                states.insert(k, quint32(r.dwState));
            }
        }
    }
    sz = 0;
    if (GetExtendedTcpTable(nullptr, &sz, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0)
            == ERROR_INSUFFICIENT_BUFFER
        && sz > 0) {
        std::vector<char> buf(sz);
        if (GetExtendedTcpTable(buf.data(), &sz, FALSE, AF_INET6,
                                TCP_TABLE_OWNER_PID_ALL, 0)
            == NO_ERROR) {
            auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCP6ROW_OWNER_PID& r = t->table[i];
                TsConnKey k;
                k.localAddr = IpKey::fromV6(r.ucLocalAddr);
                k.localPort = ntohs(uint16_t(r.dwLocalPort));
                k.remoteAddr = IpKey::fromV6(r.ucRemoteAddr);
                k.remotePort = ntohs(uint16_t(r.dwRemotePort));
                states.insert(k, quint32(r.dwState));
            }
        }
    }

    QHash<quint32, int> byState;
    int notFound = 0;
    for (const TsConnKey& k : samples) {
        auto it = states.constFind(k);
        if (it == states.constEnd()) {
            ++notFound;
            continue;
        }
        byState[it.value()] += 1;
    }

    QString s = QStringLiteral("样本=%1 快照中未找到=%2").arg(samples.size()).arg(notFound);
    for (auto it = byState.constBegin(); it != byState.constEnd(); ++it)
        s += QStringLiteral(" %1(%2):%3")
                 .arg(it.key())
                 .arg(tcpStateText(it.key()))
                 .arg(it.value());
    return s;
}
