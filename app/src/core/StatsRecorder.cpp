#include "StatsRecorder.h"

#include "Settings.h"
#include "StatsDb.h"
#include "TrafficStore.h"

#include <QDate>
#include <QDateTime>
#include <QLatin1String>
#include <QSettings>
#include <QTime>

namespace {
const int kSampleSec = 60;
const qint64 kDaySec = 86400;
const qint64 kHourSec = 3600;
const char* const kAppRetainKey = "Stats/appRetainDays";
const char* const kDomainRetainKey = "Stats/domainRetainDays";
}  // namespace

StatsRecorder& StatsRecorder::instance()
{
    static StatsRecorder s;
    return s;
}

bool StatsRecorder::start(QString* err)
{
    // 保留天数与记录粒度都在启动时读一次（只有设置弹窗会改它们）
    m_appRetainDays = readDays(kAppRetainKey);
    m_domainRetainDays = readDays(kDomainRetainKey);
    m_granularity = normalizeGranularity(ts::statsGranularity());

    if (!StatsDb::instance().open(err))
        return false;

    m_timer.setInterval(kSampleSec * 1000);
    connect(&m_timer, &QTimer::timeout, this, &StatsRecorder::onSample);
    m_timer.start();
    onSample();   // 立即采一次，把自启动以来的累计纳入统计
    return true;
}

void StatsRecorder::stop()
{
    m_timer.stop();
    if (StatsDb::instance().isOpen()) {
        onSample();   // 补记最后一分钟
        StatsDb::instance().close();
    }
}

void StatsRecorder::flushNow()
{
    // 与周期采样同一条路径：取增量 -> UPSERT 到当前小时桶。
    // 采样基准由 takeXxxSample 内部推进，重复调用不会重复计数。
    onSample();
}

// ini 里的粒度是裸整数（Settings.h 只认 1~2），非法值一律回落到小时
Granularity StatsRecorder::normalizeGranularity(int g)
{
    return g == int(Granularity::Day) ? Granularity::Day : Granularity::Hour;
}

void StatsRecorder::setGranularity(Granularity g)
{
    if (g == m_granularity)
        return;
    // 先把待落盘的增量按**旧**粒度提交：采样基准在 takeXxxSample() 里推进，
    // 之后采到的增量才会进新桶，两边不会重复计数。
    if (StatsDb::instance().isOpen())
        onSample();
    m_granularity = g;
    ts::setStatsGranularity(int(g));
}

int StatsRecorder::readDays(const char* key)
{
    bool ok = false;
    const int v = ts::appSettings()
                      .value(QLatin1String(key), kDefaultRetainDays)
                      .toInt(&ok);
    if (ok && (v == kRetainForever || v >= 1))
        return v;
    if (ok && v == 0) {
        // 旧版本用 0 表示"永久保留"：保留用户的意思，顺便归一到新的 -1 记法
        saveDays(key, kRetainForever);
        return kRetainForever;
    }
    return kDefaultRetainDays;   // ini 被手改坏（负数 / 非数字）时回落到默认
}

void StatsRecorder::saveDays(const char* key, int days)
{
    ts::setIni(QLatin1String(key), days);
}

// 只允许"正整数天数"或 kRetainForever；其余值（含语义含糊的 0）一律按永久保留处理，
// 宁可不删也不能让用户的数据凭空没了。
int StatsRecorder::normalizeRetainDays(int days)
{
    return days >= 1 ? days : kRetainForever;
}

void StatsRecorder::setAppRetainDays(int days)
{
    days = normalizeRetainDays(days);
    if (days == m_appRetainDays)
        return;
    m_appRetainDays = days;
    saveDays(kAppRetainKey, days);
    pruneNow();   // 调小立即生效；调大不需要做事
}

void StatsRecorder::setDomainRetainDays(int days)
{
    days = normalizeRetainDays(days);
    if (days == m_domainRetainDays)
        return;
    m_domainRetainDays = days;
    saveDays(kDomainRetainKey, days);
    pruneNow();
}

void StatsRecorder::clearHistory(bool app, bool domain)
{
    StatsDb& db = StatsDb::instance();
    if (!db.isOpen() || (!app && !domain))
        return;

    onSample();          // 先把待落盘的增量写进库，否则"清空"之后它又会长回来
    db.clear(app, domain);
    // 丢掉清空前的采样基准：之后落盘的都是清空之后发生的流量
    if (app)
        TrafficStore::instance().takeProcSample();
    if (domain) {
        TrafficStore::instance().takeDomainSample();
        TrafficStore::instance().takeSiteSample();   // 网站表跟域名一起清了，基准一并丢
    }
}

// 按当前保留天数清理：kRetainForever(-1) = 永久保留（传 0 给 StatsDb 表示该维度跳过）
void StatsRecorder::pruneNow()
{
    StatsDb& db = StatsDb::instance();
    if (!db.isOpen())
        return;

    const qint64 base = hourStart(QDateTime::currentDateTime());
    const qint64 appBefore = m_appRetainDays == kRetainForever
                                 ? 0
                                 : base - qint64(m_appRetainDays) * kDaySec;
    const qint64 domainBefore = m_domainRetainDays == kRetainForever
                                    ? 0
                                    : base - qint64(m_domainRetainDays) * kDaySec;
    // 网站与域名共用同一份保留天数（同一份流量的两个切片，分开设只会对不上）
    db.prune(appBefore, domainBefore, domainBefore);
}

qint64 StatsRecorder::hourStart(const QDateTime& dt)
{
    QDateTime h = dt;
    h.setTime(QTime(dt.time().hour(), 0, 0));
    return h.toSecsSinceEpoch();
}

void StatsRecorder::bucketRange(const QDateTime& dt, Granularity g,
                                qint64& start, qint64& end)
{
    if (g == Granularity::Hour) {
        start = hourStart(dt);
        end = start + kHourSec;
        return;
    }

    // 天：本地 0 点 到 次日 0 点
    const QDate d = dt.date();
    start = QDateTime(d, QTime(0, 0)).toSecsSinceEpoch();
    end = QDateTime(d.addDays(1), QTime(0, 0)).toSecsSinceEpoch();
}

void StatsRecorder::onSample()
{
    StatsDb& db = StatsDb::instance();
    if (!db.isOpen())
        return;

    qint64 start = 0;
    qint64 end = 0;
    bucketRange(QDateTime::currentDateTime(), m_granularity, start, end);

    TrafficStore& store = TrafficStore::instance();
    if (m_granularity == Granularity::Hour) {
        for (const StatDelta& d : store.takeProcSample())
            db.addApp(start, d.name, d.up, d.down);
        for (const StatDelta& d : store.takeDomainSample())
            db.addDomain(start, d.name, d.up, d.down);
        for (const StatDelta& d : store.takeSiteSample())
            db.addSite(start, d.name, d.up, d.down);
    } else {
        for (const StatDelta& d : store.takeProcSample())
            db.addAppBucket(start, end, d.name, d.up, d.down);
        for (const StatDelta& d : store.takeDomainSample())
            db.addDomainBucket(start, end, d.name, d.up, d.down);
        for (const StatDelta& d : store.takeSiteSample())
            db.addSiteBucket(start, end, d.name, d.up, d.down);
    }

    // 每天清理一次超出保留期的记录（应用 / 域名各自的天数）
    const qint64 today = QDate::currentDate().toJulianDay();
    if (m_lastPruneDay != today) {
        m_lastPruneDay = today;
        pruneNow();
    }
}
