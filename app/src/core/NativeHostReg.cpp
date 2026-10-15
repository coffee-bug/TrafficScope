#include "NativeHostReg.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace ts {

namespace {

void terminateHostProcesses()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"TrafficScopeHost.exe") == 0) {
                HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (proc != nullptr) {
                    TerminateProcess(proc, 0);
                    CloseHandle(proc);
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

// 与 extension/manifest.json 对应：
//   name = 注册表键名，也是扩展 connectNative() 传的 host 名
//   key  = 把扩展 ID 钉死（重装扩展也不变），所以 allowed_origins 可以写常量
const wchar_t* const kHostName = L"com.trafficscope.site";
const char* const kHostNameUtf8 = "com.trafficscope.site";
const char* const kExtensionId = "phdnbcbpkhpbodccdkncbpkfdlmakmcf";

// 每个浏览器一套 NativeMessagingHosts 根，值名固定为 host 名、默认值 = manifest 路径。
// 只写 HKCU（当前用户）——与 install-host.ps1 的默认行为一致；
// 机器级（-System）留给安装包/脚本，注销时我们会把 HKLM 的遗留一起清掉。
const wchar_t* const kRoots[] = {
    L"SOFTWARE\\Google\\Chrome\\NativeMessagingHosts",
    L"SOFTWARE\\Microsoft\\Edge\\NativeMessagingHosts"
};

// manifest 放在固定位置（别的程序要按绝对路径读它），
// 不能走 core/Settings.h 那套带迁移逻辑的目录。
QString manifestDir()
{
    const QString base = qEnvironmentVariable("LOCALAPPDATA");
    return QDir::toNativeSeparators(
        base.isEmpty()
            ? QCoreApplication::applicationDirPath() + QLatin1String("/native-host")
            : base + QLatin1String("/TrafficScope/native-host"));
}

QString manifestPath()
{
    return QDir::toNativeSeparators(
        manifestDir() + QLatin1Char('/') + QString::fromLatin1(kHostNameUtf8)
        + QLatin1String(".json"));
}

// host exe 与主程序同目录（同一个 build / 同一个安装目录）
QString hostExePath()
{
    return QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QLatin1String("/TrafficScopeHost.exe"));
}

QString subKeyFor(const wchar_t* root)
{
    return QString::fromWCharArray(root) + QLatin1Char('\\')
         + QString::fromLatin1(kHostNameUtf8);
}

// Qt 的 QString 与 Win32 的宽字符都是 UTF-16，直接借用缓冲（不加副本）
const wchar_t* wcs(const QString& s)
{
    return reinterpret_cast<const wchar_t*>(s.utf16());
}

bool writeRegString(HKEY root, const QString& subKey, const QString& value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, wcs(subKey), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring v = value.toStdWString();
    const LONG rc = RegSetValueExW(key, nullptr, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(v.c_str()),
                                  DWORD((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

void deleteRegTree(HKEY root, const QString& subKey)
{
    // 键不存在返回 ERROR_FILE_NOT_FOUND，这里当成功（幂等）
    RegDeleteTreeW(root, wcs(subKey));
}

}  // namespace

bool browserHostRegister(QString* err)
{
    const QString exe = hostExePath();
    if (!QFile::exists(exe)) {
        if (err)
            *err = QStringLiteral("找不到 %1（应与主程序在同一目录）").arg(exe);
        return false;
    }

    const QString dir = manifestDir();
    if (!QDir().mkpath(dir)) {
        if (err)
            *err = QStringLiteral("无法创建目录 %1").arg(dir);
        return false;
    }

    const QString mf = manifestPath();
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), QString::fromLatin1(kHostNameUtf8));
    obj.insert(QStringLiteral("description"),
               QStringLiteral("TrafficScope site attribution bridge"
                              " (browser extension -> local app)"));
    obj.insert(QStringLiteral("path"), exe);
    obj.insert(QStringLiteral("type"), QStringLiteral("stdio"));
    obj.insert(QStringLiteral("allowed_origins"),
               QJsonArray{QStringLiteral("chrome-extension://%1/")
                              .arg(QString::fromLatin1(kExtensionId))});

    QFile f(mf);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("无法写入 %1").arg(mf);
        return false;
    }
    // QJsonDocument 输出的是**无 BOM** 的 UTF-8 —— Chrome 解析 manifest 时不吃 BOM
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    f.close();

    int failed = 0;
    for (const wchar_t* root : kRoots) {
        if (!writeRegString(HKEY_CURRENT_USER, subKeyFor(root), mf))
            ++failed;
    }
    if (failed == int(sizeof(kRoots) / sizeof(kRoots[0]))) {
        if (err)
            *err = QStringLiteral("注册表写入失败（Chrome / Edge 两处都没写成功）");
        return false;
    }
    return true;
}

void browserHostUnregister()
{
    for (const wchar_t* root : kRoots) {
        const QString sub = subKeyFor(root);
        deleteRegTree(HKEY_CURRENT_USER, sub);
        deleteRegTree(HKEY_LOCAL_MACHINE, sub);   // -System 装过的话也一并清掉
    }
    QFile::remove(manifestPath());
    QDir().rmdir(manifestDir());   // 只在空目录时成功，非空静默失败（正常）
    terminateHostProcesses();
}

}  // namespace ts
