#pragma once
// 用户设置 / 运行期数据目录的统一入口。
//
// 数据与设置**不放在安装目录**，统一落在用户数据目录：
//   %LOCALAPPDATA%\TrafficScope\settings.ini        用户设置
//   %LOCALAPPDATA%\TrafficScope\data\traffic.db     SQLite 历史库 + 诊断日志
// Why：安装目录（Program Files）不该写运行期数据 —— 升级/重装容易被覆盖或清理干净。
//      放用户目录后，卸载可以选择保留数据，重装也不会丢历史。
// How：卸载时清这一处目录（见 installer/TrafficScope.iss 的卸载询问），
//      程序自己永远不删自己的数据目录。
//
// 注意：
//  1. 不要再使用无参 QSettings()（Windows 下默认写注册表
//     HKCU\Software\<Org>\<App>），一律走 ts::appSettings()。
//  2. **写设置走 ts::setIni() / ts::removeIni()**（appSettings() 的封装）。
//     ini 里**不写注释/说明**：QSettings 是"整份重写"，任何注释都会在下一次写入后
//     被冲掉，靠注释维持说明既不可靠，又是在引导用户手改配置（手改风险大，
//     可配置项一律走设置弹窗）。
//  3. 取路径依赖 QCoreApplication::applicationDirPath()（仅在 AppData 取不到时兜底用）
//     与 QApplication 的组织名，必须在 QApplication 构造完成之后调用
//     （首次调用为懒加载，正常满足）。
//  4. 程序以管理员权限运行（exe 清单声明），AppData 取到的仍是**当前登录用户**那一套，
//     不会落到 Administrator 名下 —— 这是 AppData 相对"写安装目录"的另一个好处。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QVariant>
#include <QtGlobal>

namespace ts {

// 用户数据根目录：%LOCALAPPDATA%\TrafficScope（幂等创建，已存在时无操作）。
// 设置文件与历史库都挂在它下面。
inline QString userDataRoot()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // 极端兜底（两个 AppData 都取不到）：退回程序目录，保证总有地方可写
    const QString dir = base.isEmpty()
                            ? QCoreApplication::applicationDirPath() + QLatin1String("/TrafficScope-data")
                            : base + QLatin1String("/TrafficScope");
    QDir().mkpath(dir);
    return dir;
}

// 运行期数据：<用户数据根目录>/data（SQLite 历史库与诊断日志）
inline QString dataDir()
{
    const QString dir = userDataRoot() + QLatin1String("/data");
    QDir().mkpath(dir);
    return dir;
}

// 用户设置文件：<用户数据根目录>/settings.ini
inline QString settingsPath()
{
    return userDataRoot() + QLatin1String("/settings.ini");
}

// 老版本把 settings.ini 与 data/ 放在**安装目录**下（更早的版本把 ini 放在
// data/settings.ini）。这里是搬到用户目录的一次性迁移：只在进程内第一次调用时做一次，
// 保证老用户的网卡/字体/天数与历史流量都不丢。
// 搬完源文件会被删掉，避免两处数据并存；跨盘符时退化为复制（源文件保留）。
inline void migrateFromAppDir()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    const QString appDir = QCoreApplication::applicationDirPath();

    // 1) settings.ini：根目录优先，更老的历史位置 data/settings.ini 一并覆盖
    if (!QFileInfo::exists(settingsPath())) {
        const QString cands[] = {appDir + QLatin1String("/settings.ini"),
                                 appDir + QLatin1String("/data/settings.ini")};
        for (const QString& old : cands) {
            if (!QFileInfo::exists(old))
                continue;
            if (QFile::rename(old, settingsPath()))   // 同盘符：rename 即移动
                break;
            QFile::copy(old, settingsPath());         // 跨盘符退化为复制
            break;
        }
    }

    // 2) data/ 下的文件逐个搬过去（traffic.db / -wal / -shm / 诊断日志）
    const QString oldDir = appDir + QLatin1String("/data");
    if (!QFileInfo::exists(oldDir))
        return;
    const QString newDir = dataDir();
    const QFileInfoList items = QDir(oldDir).entryInfoList(QDir::Files);
    for (const QFileInfo& fi : items) {
        const QString dst = newDir + QStringLiteral("/") + fi.fileName();
        if (QFileInfo::exists(dst))
            continue;                                // 目标已有同名文件：保留它，不动源文件
        if (!QFile::rename(fi.absoluteFilePath(), dst))
            QFile::copy(fi.absoluteFilePath(), dst);
    }
    // 搬空之后留个空目录很难看（QDir::rmdir 只在为空时成功，非空会静默失败）
    QDir().rmdir(oldDir);
}

// 用户设置（浮框位置/吸附边、详情弹窗大小、字体、保留天数等）
inline QSettings appSettings()
{
    migrateFromAppDir();
    // 必须写成"纯右值返回"：QSettings 的拷贝构造是 deleted，写成
    // `QSettings st(...); return st;` 会编译失败（具名返回值 NRVO 不保证省略）。
    // 编码：Qt6 的 ini 默认就是 UTF-8（实测中文字体名落盘字节正确），不必处理；
    // 若将来退回 Qt5（默认 Latin-1 会乱码），只能改成出参形式才能
    // 先 setIniCodec("UTF-8") 再交出去 —— QSettings 不可拷贝，这里没法那么写。
    return QSettings(settingsPath(), QSettings::IniFormat);
}

// ---- 统一写入口 ----
inline void setIni(const QString& groupKey, const QVariant& v)
{
    QSettings st = appSettings();
    st.setValue(groupKey, v);
    st.sync();
}

inline void removeIni(const QString& groupKey)
{
    QSettings st = appSettings();
    st.remove(groupKey);
    st.sync();
}

// ---- 浮框刷新周期（ms）----
// 单一来源：UI 只读/只写这里，别在别处各记一份范围
constexpr int kRefreshMsMin = 100;
constexpr int kRefreshMsMax = 10000;
constexpr int kRefreshMsDefault = 1000;

inline int clampRefreshMs(int ms)
{
    return qBound(kRefreshMsMin, ms, kRefreshMsMax);
}

// 读 settings.ini 的 [Float] refreshMs；缺失或越界（比如手改坏了）回落到默认
inline int floatRefreshMs()
{
    bool ok = false;
    const int v = appSettings()
                      .value(QStringLiteral("Float/refreshMs"), kRefreshMsDefault)
                      .toInt(&ok);
    return (ok && v >= kRefreshMsMin && v <= kRefreshMsMax) ? v : kRefreshMsDefault;
}

inline void setFloatRefreshMs(int ms)
{
    setIni(QStringLiteral("Float/refreshMs"), clampRefreshMs(ms));
}

// ---- 速率单位 ----
// 0=字节（B/s，默认，与早期版本一致）；1=比特（bps，运营商带宽口径，1000 进制）。
// 只负责**存哪个档位**；真正按它格式化的是 TrafficStore.h 的 formatRate()（单一出口）。
constexpr int kRateUnitByte = 0;
constexpr int kRateUnitBit = 1;
constexpr int kRateUnitDefault = kRateUnitByte;

inline int clampRateUnit(int u)
{
    return (u == kRateUnitBit) ? kRateUnitBit : kRateUnitByte;
}

inline int rateUnit()
{
    bool ok = false;
    const int v = appSettings()
                      .value(QStringLiteral("Float/unit"), kRateUnitDefault)
                      .toInt(&ok);
    return ok ? clampRateUnit(v) : kRateUnitDefault;
}

inline void setRateUnit(int u)
{
    setIni(QStringLiteral("Float/unit"), clampRateUnit(u));
}

// ---- 是否在悬停面板顶部显示峰值速率 ----
// 默认开启。关掉后面板顶部只剩「今日用量」一行。
inline bool showPeakRate()
{
    return appSettings().value(QStringLiteral("Float/showPeak"), true).toBool();
}

inline void setShowPeakRate(bool on)
{
    setIni(QStringLiteral("Float/showPeak"), on);
}

// ---- 检测到全屏程序时自动隐藏浮窗 ----
// 默认开启：游戏 / 放映时去掉置顶并隐藏，退出全屏后按原本的置顶意愿恢复。
// 关掉后浮框在全屏程序之上照常显示（由系统 Z 序决定，可能与全屏程序互相盖）。
inline bool hideOnFullscreen()
{
    return appSettings().value(QStringLiteral("Float/hideOnFullscreen"), true).toBool();
}

inline void setHideOnFullscreen(bool on)
{
    setIni(QStringLiteral("Float/hideOnFullscreen"), on);
}

// ---- 拖到屏幕边缘时自动贴边隐藏 ----
// 默认开启：松手时靠近某条边就吸附过去，只露出一条速率条，鼠标移上去再滑出。
// 关掉后浮框停在松手的位置、始终完整显示（不再吸附，也不再收起）。
// 关掉时已存的 [Float] edge 不再生效，但位置照旧记忆；重新打开会按当前位置再判一次。
inline bool autoEdgeHide()
{
    return appSettings().value(QStringLiteral("Float/autoEdgeHide"), true).toBool();
}

inline void setAutoEdgeHide(bool on)
{
    setIni(QStringLiteral("Float/autoEdgeHide"), on);
}

// ---- 浏览器扩展归属（native messaging host 的运行期注册）----
// 默认开启：主程序运行期间才在注册表里登记 TrafficScopeHost.exe，扩展才有资格把
// 「资源域名/IP → 站点」送进来（见 core/NativeHostReg.h 的机制说明）。
// 关掉后主程序不登记，扩展侧 connectNative 直接失败，**不会拉起任何 host 进程**，
// 「网站」维度没有数据，其它功能完全不受影响。
inline bool browserAttribution()
{
    return appSettings().value(QStringLiteral("Browser/attribution"), true).toBool();
}

inline void setBrowserAttribution(bool on)
{
    setIni(QStringLiteral("Browser/attribution"), on);
}

// ---- 是否统计"未归因流量"（pid == 0 的流量）----
// 关掉后：抓包不再把它计入任何维度（域名 / 总量 / 进程），界面也不再展示；
// **已落盘的历史数据不动**（只过滤不删除）。
// 默认**关掉统计**（2026-10-11 用户要求）：设置里「不再统计未知流量」默认勾选，
// 未知流量不计入总量，界面读数因此比任务管理器偏小。老用户 ini 里已有该键的，
// 仍按自己存的值走，不受影响。
inline bool countUnattributed()
{
    return appSettings().value(QStringLiteral("Stats/countUnattributed"), false).toBool();
}

inline void setCountUnattributed(bool on)
{
    setIni(QStringLiteral("Stats/countUnattributed"), on);
}

// ---- 历史记录粒度 ----
// 1=小时 2=天（对应 StatsDb.h 的 Granularity）。默认小时，
// 与老版本行为一致，升级上来的用户不会有任何变化。
// **只对设置生效之后新落盘的数据起作用**：已有记录保持原粒度不动。
inline int statsGranularity()
{
    bool ok = false;
    const int v = appSettings()
                      .value(QStringLiteral("Stats/granularity"), 1)
                      .toInt(&ok);
    return (ok && v >= 1 && v <= 2) ? v : 1;
}

inline void setStatsGranularity(int g)
{
    setIni(QStringLiteral("Stats/granularity"), (g >= 1 && g <= 2) ? g : 1);
}

// ---- 界面字体 ----
// settings.ini 的 [UI] fontFamily；空/缺失 = 系统默认（Segoe UI）
inline const QString& defaultFontFamily()
{
    static const QString f = QStringLiteral("Segoe UI");
    return f;
}

inline QString uiFontFamily()
{
    const QString f = appSettings()
                          .value(QStringLiteral("UI/fontFamily"))
                          .toString()
                          .trimmed();
    return f.isEmpty() ? defaultFontFamily() : f;
}

inline void setUiFontFamily(const QString& family)
{
    const QString f = family.trimmed();
    if (f.isEmpty())
        removeIni(QStringLiteral("UI/fontFamily"));
    else
        setIni(QStringLiteral("UI/fontFamily"), f);
}

}  // namespace ts
