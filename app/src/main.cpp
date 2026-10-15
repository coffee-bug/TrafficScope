#include <QApplication>
#include <QDebug>
#include <QFont>
#include <QMessageBox>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>

#include "core/EtwSniffer.h"
#include "core/NativeHost.h"
#include "core/NativeHostReg.h"
#include "core/PacketSource.h"
#include "core/SingleInstance.h"
#include "core/StatsRecorder.h"
#include "core/TrafficStore.h"
#include "ui/AppIcon.h"
#include "ui/FloatWidget.h"
#include "ui/UiTheme.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TrafficScope"));
    app.setOrganizationName(QStringLiteral("TrafficScope"));

    // 单实例保护：第二个实例只负责把已有实例的浮框带到前台，然后退出。
    // 必须放在一切共享资源启动之前 —— 否则第二个实例会跟已有实例抢
    // Npcap 抓包、ETW 会话和 traffic.db（SQLite），数字翻倍且互相踩。
    ts::SingleInstance single;
    QString singleErr;
    if (!single.tryStart(QStringLiteral("TrafficScope"), &singleErr)) {
        if (!single.alreadyRunning()) {
            // 连不上也起不来的兜底（极少见的竞争），留个日志再退
            qWarning() << "TrafficScope: single-instance guard failed:" << singleErr;
        }
        return 0;
    }

    // 界面字体：settings.ini 的 [UI] fontFamily（默认 Segoe UI）
    app.setFont(QFont(ts::uiFontFamily(), 9));
    ts::setFontFamily(ts::uiFontFamily());   // 同步到主题（手绘部分与 QSS 都读它）

    // 是否统计"未知流量"（settings.ini 的 [Stats] countUnattributed，默认不统计）
    TrafficStore::instance().setCountsUnattributed(ts::countUnattributed());

    // 速率显示单位：0=字节 B/s（默认）/ 1=比特 bps。全局唯一出口在 formatRate()
    setRateUnitMode(ts::rateUnit());
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);
    // 与 UI 主题同源：原生控件（消息框等）也跟着当前主题走
    app.setPalette(ts::theme().palette());

    // 程序图标（↑橙 ↓绿，与托盘同一枚）：窗口、任务栏、Alt+Tab、自绘标题栏都用它。
    // 箭头按主题色现画，所以换主题后要重设一次（exe 文件那枚是 .ico 资源，不参与）。
    app.setWindowIcon(ts::appIcon());
    QObject::connect(&ts::themeNotifier(), &ts::ThemeNotifier::changed, &app,
                     [&app]() { app.setWindowIcon(ts::appIcon()); });

    // 历史统计：数据库失败只影响「历史累计」，不阻断实时流量监控
    QString dbErr;
    if (!StatsRecorder::instance().start(&dbErr))
        QMessageBox::warning(nullptr, QStringLiteral("TrafficScope"),
            QStringLiteral("历史流量统计不可用（数据库打开失败）：\n%1").arg(dbErr));

    // 浏览器扩展归属通道（二期，可选）：监听本机管道，收 TrafficScopeHost.exe 转来的
    // 「资源域名/IP → 站点域名」。起不来不影响任何其它功能，只是网站维度少了最准的那条线索。
    QString bridgeErr;
    if (!ts::SiteBridge::instance().listen(&bridgeErr))
        qWarning() << "TrafficScope: site bridge unavailable:" << bridgeErr;

    // native host 的注册跟着主程序的生命周期走：**登记存在 = "主程序在跑"**，
    // 浏览器才有资格去 spawn TrafficScopeHost.exe。所以：
    //   启动先无条件清一次（自愈上次崩溃留下的键）→ 按设置决定是否登记；
    //   退出（aboutToQuit）注销 —— 程序没跑时扩展的连接会直接失败且不产生进程。
    ts::browserHostUnregister();
    if (ts::browserAttribution()) {
        QString regErr;
        if (!ts::browserHostRegister(&regErr))
            qWarning() << "TrafficScope: native host register failed:" << regErr;
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                     []() { ts::browserHostUnregister(); });

    // ETW 归因：订阅内核 TCP/IP 事件，实时维护「四元组 → PID」（ProcTraffic::pidFor 第一级），
    // 失败不影响主流程（原因见诊断日志）。
    EtwSniffer::instance().start(nullptr);

    // 每 2 秒把 ETW 统计刷进诊断日志。日志默认关闭（只在调试时开，见 EtwSniffer::probeEnabled），
    // 所以定时器也只在开启时启动 —— 交付版不会产生 data/etw_probe.log。
    QTimer etwStatsTimer;
    if (EtwSniffer::probeEnabled()) {
        etwStatsTimer.setInterval(2000);
        QObject::connect(&etwStatsTimer, &QTimer::timeout, &etwStatsTimer,
                         []() { EtwSniffer::instance().dumpStats(); });
        etwStatsTimer.start();
    }

    PacketSource& source = PacketSource::instance();

    QString err;
    if (!source.startCapture(err)) {
        QMessageBox::warning(nullptr, QStringLiteral("TrafficScope"),
            QStringLiteral("启动抓包失败：\n%1\n\n请确认：\n"
                           "1. 本程序已以管理员身份运行（需具备启动内核跟踪权限）；\n"
                           "2. 系统环境支持 Windows NDIS 抓包提供程序。").arg(err));
        return 1;
    }

    // captureError 是在抓包线程（PacketSource::run）里发出的：必须显式给一个
    // 主线程的 context（app），AutoConnection 才会走队列连接到 GUI 线程；
    // 否则 lambda 会在抓包线程里直接操作 QMessageBox（跨线程碰 GUI）。
    QObject::connect(&source, &PacketSource::captureError, &app,
                     [](const QString& msg) {
                         QMessageBox::warning(nullptr, QStringLiteral("TrafficScope"),
                                              QStringLiteral("抓包异常：\n%1").arg(msg));
                     });

    FloatWidget w;
    // 收到第二实例的 show 消息时把浮框带到前台（浮框是置顶的 Qt::Tool，
    // show + raise + activateWindow 已足够；被全屏隐藏时 showNormal 会把它唤回）
    single.onActivate([&w]() { w.showNormal(); w.raise(); w.activateWindow(); });
    w.show();

    const int ret = app.exec();
    source.stopCapture();
    EtwSniffer::instance().stop();
    StatsRecorder::instance().stop();
    return ret;
}
