#include "StatsDb.h"

#include "Settings.h"

#include <QDir>
#include <QLatin1String>

#include <winsqlite/winsqlite3.h>

namespace {

// RAII 辅助类：自动析构 sqlite3_stmt*
class Stmt {
public:
    Stmt(sqlite3* db, const QString& sql)
    {
        const QByteArray utf8 = sql.toUtf8();
        const int rc = sqlite3_prepare_v2(db, utf8.constData(), utf8.size(), &m_stmt, nullptr);
        if (rc != SQLITE_OK) {
            qWarning("StatsDb: prepare failed (%d: %s): %s",
                     rc, sqlite3_errmsg(db), utf8.constData());
            m_stmt = nullptr;
        }
    }

    ~Stmt()
    {
        if (m_stmt) {
            sqlite3_finalize(m_stmt);
        }
    }

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    sqlite3_stmt* get() const { return m_stmt; }
    operator sqlite3_stmt*() const { return m_stmt; }
    bool isValid() const { return m_stmt != nullptr; }

    bool bindInt64(int idx, qint64 val)
    {
        return m_stmt && sqlite3_bind_int64(m_stmt, idx, val) == SQLITE_OK;
    }

    bool bindText(int idx, const QString& text)
    {
        if (!m_stmt) return false;
        m_cachedUtf8 = text.toUtf8();
        return sqlite3_bind_text(m_stmt, idx, m_cachedUtf8.constData(),
                                 m_cachedUtf8.size(), SQLITE_TRANSIENT) == SQLITE_OK;
    }

private:
    sqlite3_stmt* m_stmt = nullptr;
    QByteArray m_cachedUtf8;
};

bool execSql(sqlite3* db, const char* sql)
{
    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        qWarning("StatsDb: exec failed (%s): %s", sql, errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

}  // namespace

StatsDb& StatsDb::instance()
{
    static StatsDb s;
    return s;
}

bool StatsDb::open(QString* err)
{
    if (m_ok && m_db)
        return true;

    // 数据目录 = 用户数据目录下的 data/（%LOCALAPPDATA%\TrafficScope\data，见 core/Settings.h）
    const QString dir = ts::dataDir();
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        if (err)
            *err = QStringLiteral("无法创建数据目录: %1").arg(dir);
        return false;
    }
    m_path = dir + QLatin1String("/traffic.db");

    // 用 UTF-8 路径打开数据库
    const QByteArray pathUtf8 = m_path.toUtf8();
    const int rc = sqlite3_open_v2(
        pathUtf8.constData(),
        &m_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        nullptr
    );

    if (rc != SQLITE_OK) {
        if (err) {
            *err = QStringLiteral("无法打开数据库，请确认有写权限（建议以管理员身份运行）：\n%1")
                       .arg(m_db ? QString::fromUtf8(sqlite3_errmsg(m_db)) : QString::number(rc));
        }
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        return false;
    }

    execSql(m_db, "PRAGMA journal_mode = WAL;");

    m_ok = ensureTables();
    if (!m_ok && err)
        *err = QStringLiteral("初始化数据表失败");
    return m_ok;
}

void StatsDb::close()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
    m_ok = false;
}

bool StatsDb::ensureTables()
{
    if (!m_db)
        return false;

    const char* const sqls[] = {
        "CREATE TABLE IF NOT EXISTS hourly_app ("
        " hour_ts INTEGER NOT NULL, app TEXT NOT NULL,"
        " up INTEGER NOT NULL DEFAULT 0, down INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(hour_ts, app)) WITHOUT ROWID;",

        "CREATE TABLE IF NOT EXISTS hourly_domain ("
        " hour_ts INTEGER NOT NULL, domain TEXT NOT NULL,"
        " up INTEGER NOT NULL DEFAULT 0, down INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(hour_ts, domain)) WITHOUT ROWID;",

        // 粗粒度桶（天 / 周 / 月）：多存一个 end_ts，查询时才知道这个桶覆盖到哪
        "CREATE TABLE IF NOT EXISTS bucket_app ("
        " start_ts INTEGER NOT NULL, end_ts INTEGER NOT NULL,"
        " app TEXT NOT NULL,"
        " up INTEGER NOT NULL DEFAULT 0, down INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(start_ts, app)) WITHOUT ROWID;",

        "CREATE TABLE IF NOT EXISTS bucket_domain ("
        " start_ts INTEGER NOT NULL, end_ts INTEGER NOT NULL,"
        " domain TEXT NOT NULL,"
        " up INTEGER NOT NULL DEFAULT 0, down INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(start_ts, domain)) WITHOUT ROWID;",

        // 网站维度（2026-10-12 起）：落库时的名字就是"当时归属到的网站"，
        // 归不到的以裸 IP 名落库 —— 老数据不会回填，网站维度的历史从这一刻开始积累
        "CREATE TABLE IF NOT EXISTS hourly_site ("
        " hour_ts INTEGER NOT NULL, site TEXT NOT NULL,"
        " up INTEGER NOT NULL DEFAULT 0, down INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(hour_ts, site)) WITHOUT ROWID;",

        "CREATE TABLE IF NOT EXISTS bucket_site ("
        " start_ts INTEGER NOT NULL, end_ts INTEGER NOT NULL,"
        " site TEXT NOT NULL,"
        " up INTEGER NOT NULL DEFAULT 0, down INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(start_ts, site)) WITHOUT ROWID;",

        "CREATE INDEX IF NOT EXISTS idx_app_ts ON hourly_app(hour_ts);",
        "CREATE INDEX IF NOT EXISTS idx_domain_ts ON hourly_domain(hour_ts);",
        "CREATE INDEX IF NOT EXISTS idx_site_ts ON hourly_site(hour_ts);",
        "CREATE INDEX IF NOT EXISTS idx_bucket_app_ts ON bucket_app(start_ts);",
        "CREATE INDEX IF NOT EXISTS idx_bucket_domain_ts ON bucket_domain(start_ts);",
        "CREATE INDEX IF NOT EXISTS idx_bucket_site_ts ON bucket_site(start_ts);"
    };

    for (const char* sql : sqls) {
        if (!execSql(m_db, sql))
            return false;
    }
    return true;
}

void StatsDb::addApp(qint64 hourTs, const QString& name, quint64 up, quint64 down)
{
    addRow(QStringLiteral("hourly_app"), QStringLiteral("app"), hourTs, name, up, down);
}

void StatsDb::addDomain(qint64 hourTs, const QString& name, quint64 up, quint64 down)
{
    addRow(QStringLiteral("hourly_domain"), QStringLiteral("domain"), hourTs, name, up, down);
}

void StatsDb::addSite(qint64 hourTs, const QString& name, quint64 up, quint64 down)
{
    addRow(QStringLiteral("hourly_site"), QStringLiteral("site"), hourTs, name, up, down);
}

void StatsDb::addAppBucket(qint64 startTs, qint64 endTs, const QString& name,
                           quint64 up, quint64 down)
{
    addBucketRow(QStringLiteral("bucket_app"), QStringLiteral("app"),
                 startTs, endTs, name, up, down);
}

void StatsDb::addDomainBucket(qint64 startTs, qint64 endTs, const QString& name,
                              quint64 up, quint64 down)
{
    addBucketRow(QStringLiteral("bucket_domain"), QStringLiteral("domain"),
                 startTs, endTs, name, up, down);
}

void StatsDb::addSiteBucket(qint64 startTs, qint64 endTs, const QString& name,
                            quint64 up, quint64 down)
{
    addBucketRow(QStringLiteral("bucket_site"), QStringLiteral("site"),
                 startTs, endTs, name, up, down);
}

void StatsDb::addBucketRow(const QString& table, const QString& col, qint64 startTs,
                           qint64 endTs, const QString& name, quint64 up, quint64 down)
{
    if (!m_ok || !m_db || name.isEmpty() || (up == 0 && down == 0) || endTs <= startTs)
        return;

    const QString sql = QStringLiteral(
        "INSERT INTO %1(start_ts, end_ts, %2, up, down)"
        " VALUES(?, ?, ?, ?, ?) "
        "ON CONFLICT(start_ts, %2) DO UPDATE SET up = up + excluded.up,"
        " down = down + excluded.down").arg(table, col);

    Stmt s(m_db, sql);
    if (!s.isValid())
        return;

    s.bindInt64(1, startTs);
    s.bindInt64(2, endTs);
    s.bindText(3, name);
    s.bindInt64(4, static_cast<qint64>(up));
    s.bindInt64(5, static_cast<qint64>(down));

    const int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE) {
        qWarning("StatsDb: addBucketRow(%s) failed (%d): %s",
                 qPrintable(table), rc, sqlite3_errmsg(m_db));
    }
}

void StatsDb::addRow(const QString& table, const QString& col, qint64 hourTs,
                     const QString& name, quint64 up, quint64 down)
{
    if (!m_ok || !m_db || name.isEmpty() || (up == 0 && down == 0))
        return;

    const QString sql = QStringLiteral(
        "INSERT INTO %1(hour_ts, %2, up, down) VALUES(?, ?, ?, ?) "
        "ON CONFLICT(hour_ts, %2) DO UPDATE SET up = up + excluded.up,"
        " down = down + excluded.down").arg(table, col);

    Stmt s(m_db, sql);
    if (!s.isValid())
        return;

    s.bindInt64(1, hourTs);
    s.bindText(2, name);
    s.bindInt64(3, static_cast<qint64>(up));
    s.bindInt64(4, static_cast<qint64>(down));

    const int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE) {
        qWarning("StatsDb: addRow(%s) failed (%d): %s",
                 qPrintable(table), rc, sqlite3_errmsg(m_db));
    }
}

QVector<StatsDb::Row> StatsDb::queryApps(qint64 fromTs, qint64 toTs, int limit)
{
    return queryBy(QStringLiteral("hourly_app"), QStringLiteral("bucket_app"),
                   QStringLiteral("app"), fromTs, toTs, limit);
}

QVector<StatsDb::Row> StatsDb::queryDomains(qint64 fromTs, qint64 toTs, int limit)
{
    return queryBy(QStringLiteral("hourly_domain"), QStringLiteral("bucket_domain"),
                   QStringLiteral("domain"), fromTs, toTs, limit);
}

QVector<StatsDb::Row> StatsDb::querySites(qint64 fromTs, qint64 toTs, int limit)
{
    return queryBy(QStringLiteral("hourly_site"), QStringLiteral("bucket_site"),
                   QStringLiteral("site"), fromTs, toTs, limit);
}

QVector<StatsDb::Row> StatsDb::queryBy(const QString& hourTable, const QString& bucketTable,
                                       const QString& col, qint64 fromTs, qint64 toTs,
                                       int limit)
{
    QVector<Row> out;
    if (!m_ok || !m_db)
        return out;

    // 小时表与粗粒度表可能同时有数据（老数据按小时、新数据按天/周/月），两表合并求和。
    // 命中条件是"桶区间与查询区间相交"：
    //   · 小时桶的跨度固定 3600s，所以写成 hour_ts + 3600 > from；
    //   · 粗粒度桶直接用存下来的 end_ts。
    // 只用"桶起点落在区间内"会把起点更早的粗粒度桶整段漏掉。
    const QString sql = QStringLiteral(
        "SELECT %1, SUM(up), SUM(down) FROM ("
        " SELECT %1, up, down FROM %2 WHERE hour_ts < ? AND hour_ts + 3600 > ?"
        " UNION ALL"
        " SELECT %1, up, down FROM %3 WHERE start_ts < ? AND end_ts > ?"
        ") t GROUP BY %1 ORDER BY SUM(up) + SUM(down) DESC LIMIT ?")
        .arg(col, hourTable, bucketTable);

    Stmt s(m_db, sql);
    if (!s.isValid())
        return out;

    s.bindInt64(1, toTs);
    s.bindInt64(2, fromTs);
    s.bindInt64(3, toTs);
    s.bindInt64(4, fromTs);
    s.bindInt64(5, limit);

    while (sqlite3_step(s) == SQLITE_ROW) {
        Row r;
        const unsigned char* text = sqlite3_column_text(s, 0);
        r.name = text ? QString::fromUtf8(reinterpret_cast<const char*>(text)) : QString();
        r.up = static_cast<quint64>(sqlite3_column_int64(s, 1));
        r.down = static_cast<quint64>(sqlite3_column_int64(s, 2));
        out.append(r);
    }

    return out;
}

StatsDb::Totals StatsDb::totals(qint64 fromTs, qint64 toTs, bool byApp)
{
    Totals t;
    if (!m_ok || !m_db)
        return t;

    const QString hourTable = byApp ? QStringLiteral("hourly_app")
                                    : QStringLiteral("hourly_domain");
    const QString bucketTable = byApp ? QStringLiteral("bucket_app")
                                      : QStringLiteral("bucket_domain");

    const QString sql = QStringLiteral(
        "SELECT SUM(up), SUM(down) FROM ("
        " SELECT up, down FROM %1 WHERE hour_ts < ? AND hour_ts + 3600 > ?"
        " UNION ALL"
        " SELECT up, down FROM %2 WHERE start_ts < ? AND end_ts > ?"
        ") t").arg(hourTable, bucketTable);

    Stmt s(m_db, sql);
    if (!s.isValid())
        return t;

    s.bindInt64(1, toTs);
    s.bindInt64(2, fromTs);
    s.bindInt64(3, toTs);
    s.bindInt64(4, fromTs);

    if (sqlite3_step(s) == SQLITE_ROW) {
        t.up = static_cast<quint64>(sqlite3_column_int64(s, 0));
        t.down = static_cast<quint64>(sqlite3_column_int64(s, 1));
    }

    return t;
}

QVector<StatsDb::NamePoint> StatsDb::seriesByName(qint64 fromTs, qint64 toTs, int dim,
                                                 qint64 bucketSec, qint64 utcOffsetSec)
{
    QVector<NamePoint> out;
    if (!m_ok || !m_db || bucketSec <= 0 || toTs <= fromTs)
        return out;

    const QString hourTable = dim == 0 ? QStringLiteral("hourly_app")
                              : dim == 2 ? QStringLiteral("hourly_site")
                                         : QStringLiteral("hourly_domain");
    const QString bucketTable = dim == 0 ? QStringLiteral("bucket_app")
                                : dim == 2 ? QStringLiteral("bucket_site")
                                           : QStringLiteral("bucket_domain");
    const QString col = dim == 0 ? QStringLiteral("app")
                      : dim == 2 ? QStringLiteral("site")
                                 : QStringLiteral("domain");

    // 两张表各自归桶后合并。桶起点按本地时间对齐：
    //   ((ts + off) / bucket) * bucket - off
    const QString sql = QStringLiteral(
        "SELECT b, %1, SUM(up), SUM(down) FROM ("
        " SELECT ((hour_ts + ?) / ?) * ? - ? AS b, %1, up, down FROM %2"
        "  WHERE hour_ts < ? AND hour_ts + 3600 > ?"
        " UNION ALL"
        " SELECT ((start_ts + ?) / ?) * ? - ? AS b, %1, up, down FROM %3"
        "  WHERE start_ts < ? AND end_ts > ?"
        ") t GROUP BY b, %1 ORDER BY b ASC")
        .arg(col, hourTable, bucketTable);

    Stmt s(m_db, sql);
    if (!s.isValid())
        return out;

    // 每条分支六个占位符：(off, bucket, bucket, off, to, from)
    int bindIdx = 1;
    for (int i = 0; i < 2; ++i) {
        s.bindInt64(bindIdx++, utcOffsetSec);
        s.bindInt64(bindIdx++, bucketSec);
        s.bindInt64(bindIdx++, bucketSec);
        s.bindInt64(bindIdx++, utcOffsetSec);
        s.bindInt64(bindIdx++, toTs);
        s.bindInt64(bindIdx++, fromTs);
    }

    while (sqlite3_step(s) == SQLITE_ROW) {
        NamePoint p;
        p.ts = sqlite3_column_int64(s, 0);
        const unsigned char* text = sqlite3_column_text(s, 1);
        p.name = text ? QString::fromUtf8(reinterpret_cast<const char*>(text)) : QString();
        p.up = static_cast<quint64>(sqlite3_column_int64(s, 2));
        p.down = static_cast<quint64>(sqlite3_column_int64(s, 3));
        out.append(p);
    }

    return out;
}

qint64 StatsDb::earliestTs()
{
    if (!m_ok || !m_db)
        return 0;

    qint64 best = 0;
    const char* const sqls[] = {
        "SELECT MIN(hour_ts) FROM hourly_app;",
        "SELECT MIN(hour_ts) FROM hourly_domain;",
        "SELECT MIN(hour_ts) FROM hourly_site;",
        "SELECT MIN(start_ts) FROM bucket_app;",
        "SELECT MIN(start_ts) FROM bucket_domain;",
        "SELECT MIN(start_ts) FROM bucket_site;"
    };

    for (const char* sql : sqls) {
        Stmt s(m_db, QString::fromLatin1(sql));
        if (s.isValid() && sqlite3_step(s) == SQLITE_ROW) {
            if (sqlite3_column_type(s, 0) != SQLITE_NULL) {
                const qint64 v = sqlite3_column_int64(s, 0);
                if (v > 0 && (best == 0 || v < best))
                    best = v;
            }
        }
    }
    return best;
}

void StatsDb::clear(bool app, bool domain)
{
    if (!m_ok || !m_db || (!app && !domain))
        return;

    const struct { const char* table; bool on; } jobs[] = {
        {"hourly_app", app},
        {"hourly_domain", domain},
        {"hourly_site", domain},
        {"bucket_app", app},
        {"bucket_domain", domain},
        {"bucket_site", domain}
    };

    for (const auto& j : jobs) {
        if (!j.on)
            continue;
        const QString sql = QStringLiteral("DELETE FROM %1").arg(QLatin1String(j.table));
        execSql(m_db, sql.toUtf8().constData());
    }

    // 回收文件空间
    execSql(m_db, "VACUUM;");
}

void StatsDb::prune(qint64 appBeforeTs, qint64 domainBeforeTs, qint64 siteBeforeTs)
{
    if (!m_ok || !m_db)
        return;

    const struct {
        const char* table;
        qint64 beforeTs;
        bool bucket;
    } jobs[] = {
        {"hourly_app", appBeforeTs, false},
        {"hourly_domain", domainBeforeTs, false},
        {"hourly_site", siteBeforeTs, false},
        {"bucket_app", appBeforeTs, true},
        {"bucket_domain", domainBeforeTs, true},
        {"bucket_site", siteBeforeTs, true}
    };

    for (const auto& j : jobs) {
        if (j.beforeTs <= 0)
            continue;

        const QString sql = j.bucket
            ? QStringLiteral("DELETE FROM %1 WHERE end_ts <= ?").arg(QLatin1String(j.table))
            : QStringLiteral("DELETE FROM %1 WHERE hour_ts < ?").arg(QLatin1String(j.table));

        Stmt s(m_db, sql);
        if (s.isValid()) {
            s.bindInt64(1, j.beforeTs);
            const int rc = sqlite3_step(s);
            if (rc != SQLITE_DONE) {
                qWarning("StatsDb: prune %s failed (%d): %s",
                         j.table, rc, sqlite3_errmsg(m_db));
            }
        }
    }
}
