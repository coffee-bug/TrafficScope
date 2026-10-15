#include "AutoStart.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // CREATE_NO_WINDOW

#include <QCoreApplication>
#include <QDir>
#include <QLatin1String>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace ts {
namespace {

const char* const kTaskName = "TrafficScope";
const char* const kTool = "schtasks.exe";

// schtasks.exe 是控制台程序：从 GUI 进程拉起时 Windows 会给它分配一个控制台，
// 屏幕上一闪而过很难看。CREATE_NO_WINDOW 把它彻底藏掉。
void noConsoleWindow(QProcess& p)
{
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
}

bool run(QStringList args, int timeoutMs, QString* err)
{
    QProcess p;
    noConsoleWindow(p);
    p.start(QLatin1String(kTool), args);
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        if (err)
            *err = QStringLiteral("schtasks 执行超时");
        return false;
    }
    const bool ok = (p.exitStatus() == QProcess::NormalExit) && (p.exitCode() == 0);
    if (!ok && err) {
        const QString out = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
        const QString e = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
        *err = e.isEmpty() ? out : e;
        if (err->isEmpty())
            *err = QStringLiteral("schtasks 退出码 %1").arg(p.exitCode());
    }
    return ok;
}

}  // namespace

bool autoStartEnabled()
{
    // 只关心"存在与否"：schtasks /Query 找不到任务时退出码非 0
    return run(QStringList{QStringLiteral("/Query"), QStringLiteral("/TN"),
                           QLatin1String(kTaskName)},
               3000, nullptr);
}

bool setAutoStart(bool on, QString* err)
{
    QStringList args;
    if (on) {
        // /RL HIGHEST 是关键：任务以最高权限运行（进程必须是提权的，我们本来就是）
        // /SC ONLOGON：用户登录时触发；/F：已存在就覆盖（exe 路径变了重新勾选即可）
        args << QStringLiteral("/Create")
             << QStringLiteral("/TN") << QLatin1String(kTaskName)
             << QStringLiteral("/TR")
             << (QLatin1Char('"')
                 + QDir::toNativeSeparators(QCoreApplication::applicationFilePath())
                 + QLatin1Char('"'))
             << QStringLiteral("/SC") << QStringLiteral("ONLOGON")
             << QStringLiteral("/RL") << QStringLiteral("HIGHEST")
             << QStringLiteral("/F");
    } else {
        args << QStringLiteral("/Delete")
             << QStringLiteral("/TN") << QLatin1String(kTaskName)
             << QStringLiteral("/F");
    }
    return run(args, 5000, err);
}

}  // namespace ts
