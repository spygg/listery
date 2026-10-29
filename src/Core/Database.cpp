#include "Core/Database.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVariant>

namespace {
QString selectColumns()
{
    return QStringLiteral(
        "e.path, e.display_name, e.dir, e.ext, e.size, e.mtime, "
        "COALESCE(u.launch_count, 0), COALESCE(u.last_used, 0)");
}

QString selectFrom()
{
    return QStringLiteral(" FROM executables e "
                          "LEFT JOIN usage_stats u ON u.path = e.path");
}

ExecutableRecord readRecord(const QSqlQuery &query)
{
    ExecutableRecord record;
    record.path = query.value(0).toString();
    record.displayName = query.value(1).toString();
    record.dir = query.value(2).toString();
    record.ext = query.value(3).toString();
    record.size = query.value(4).toLongLong();
    record.mtime = query.value(5).toLongLong();
    record.launchCount = query.value(6).toInt();
    record.lastUsed = query.value(7).toLongLong();
    return record;
}
} // namespace

Database::Database(const QString &databasePath)
    : m_databasePath(databasePath)
    , m_connectionName(QStringLiteral("listery_%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

Database::~Database()
{
    close();
}

bool Database::open(QString *errorMessage)
{
    if (m_db && m_db->isOpen())
        return true;

    const QFileInfo dbInfo(m_databasePath);
    QDir().mkpath(dbInfo.absolutePath());

    m_db = new QSqlDatabase(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName));
    m_db->setDatabaseName(m_databasePath);
    if (!m_db->open()) {
        m_lastError = m_db->lastError().text();
        if (errorMessage)
            *errorMessage = m_lastError;
        return false;
    }

    QSqlQuery pragma(*m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
    pragma.exec(QStringLiteral("PRAGMA temp_store = MEMORY"));
    pragma.exec(QStringLiteral("PRAGMA cache_size = -20000"));

    return createSchema(errorMessage);
}

bool Database::createSchema(QString *errorMessage)
{
    QSqlQuery query(*m_db);

    const QStringList statements = QStringList()
        << QStringLiteral(
               "CREATE TABLE IF NOT EXISTS executables ("
               " path TEXT NOT NULL PRIMARY KEY,"
               " name TEXT NOT NULL,"
               " display_name TEXT NOT NULL,"
               " dir TEXT NOT NULL,"
               " ext TEXT NOT NULL,"
               " size INTEGER NOT NULL DEFAULT 0,"
               " mtime INTEGER NOT NULL DEFAULT 0,"
               " drive TEXT NOT NULL DEFAULT '',"
               " gen INTEGER NOT NULL DEFAULT 0"
               ")")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_name ON executables(name)")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_dir ON executables(dir)")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_gen ON executables(gen)")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_mtime ON executables(mtime)")
        << QStringLiteral(
               "CREATE TABLE IF NOT EXISTS usage_stats ("
               " path TEXT NOT NULL PRIMARY KEY,"
               " launch_count INTEGER NOT NULL DEFAULT 0,"
               " last_used INTEGER NOT NULL DEFAULT 0"
               ")")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_usage_last ON usage_stats(last_used DESC)")
        << QStringLiteral(
               "CREATE TABLE IF NOT EXISTS app_meta ("
               " key TEXT NOT NULL PRIMARY KEY,"
               " value TEXT NOT NULL DEFAULT ''"
               ")");

    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            if (errorMessage)
                *errorMessage = m_lastError;
            return false;
        }
    }
    return true;
}

void Database::close()
{
    if (m_upsertQuery)
        m_upsertQuery.reset();

    if (m_db) {
        if (m_db->isOpen())
            m_db->close();
        const QString name = m_connectionName;
        delete m_db;
        m_db = nullptr;
        QSqlDatabase::removeDatabase(name);
    }
}

bool Database::isOpen() const
{
    return m_db && m_db->isOpen();
}

bool Database::beginBulk()
{
    if (!isOpen())
        return false;

    if (!m_upsertQuery) {
        m_upsertQuery.reset(new QSqlQuery(*m_db));
        m_upsertQuery->prepare(
            QStringLiteral("INSERT OR REPLACE INTO executables"
                           " (path, name, display_name, dir, ext, size, mtime, drive, gen)"
                           " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    }
    return m_db->transaction();
}

bool Database::upsertExecutable(const ExecutableRecord &record, qint64 generation)
{
    if (!m_upsertQuery)
        return false;

    const QFileInfo info(record.path);
    m_upsertQuery->bindValue(0, QDir::toNativeSeparators(record.path));
    m_upsertQuery->bindValue(1, record.displayName.toLower());
    m_upsertQuery->bindValue(2, record.displayName);
    m_upsertQuery->bindValue(3, record.dir.toLower());
    m_upsertQuery->bindValue(4, record.ext.toLower());
    m_upsertQuery->bindValue(5, record.size);
    m_upsertQuery->bindValue(6, record.mtime);
    m_upsertQuery->bindValue(7, info.absolutePath().left(2).toUpper());
    m_upsertQuery->bindValue(8, generation);
    return m_upsertQuery->exec();
}

bool Database::commitBulk()
{
    if (!isOpen())
        return false;
    return m_db->commit();
}

bool Database::rollbackBulk()
{
    if (!isOpen())
        return false;
    return m_db->rollback();
}

int Database::removeStale(qint64 generation, QString *errorMessage)
{
    if (!isOpen())
        return 0;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("DELETE FROM executables WHERE gen <> ?"));
    query.addBindValue(generation);
    if (!query.exec()) {
        if (errorMessage)
            *errorMessage = query.lastError().text();
        return -1;
    }
    return query.numRowsAffected();
}

int Database::totalCount() const
{
    if (!isOpen())
        return 0;

    QSqlQuery query(*m_db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM executables")))
        return 0;
    if (query.next())
        return query.value(0).toInt();
    return 0;
}

bool Database::clearAll()
{
    if (!isOpen())
        return false;
    QSqlQuery query(*m_db);
    return query.exec(QStringLiteral("DELETE FROM executables"));
}

QVector<ExecutableRecord> Database::searchByPrefix(const QString &prefix, int limit) const
{
    QVector<ExecutableRecord> results;
    if (!isOpen() || prefix.isEmpty())
        return results;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT ") + selectColumns() + selectFrom()
                  + QStringLiteral(" WHERE e.name LIKE ? ESCAPE '\\' LIMIT ?"));
    query.addBindValue(likeEscape(prefix) + QLatin1Char('%'));
    query.addBindValue(limit);
    if (!query.exec())
        return results;

    while (query.next())
        results.append(readRecord(query));
    return results;
}

QVector<ExecutableRecord> Database::searchByTokens(const QStringList &tokens, int limit) const
{
    QVector<ExecutableRecord> results;
    if (!isOpen() || tokens.isEmpty())
        return results;

    QStringList clauses;
    for (int i = 0; i < tokens.size(); ++i)
        clauses.append(QStringLiteral("(e.name LIKE ? ESCAPE '\\' OR e.dir LIKE ? ESCAPE '\\')"));

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT ") + selectColumns() + selectFrom()
                  + QStringLiteral(" WHERE ") + clauses.join(QStringLiteral(" AND "))
                  + QStringLiteral(" LIMIT ?"));

    for (const QString &token : tokens) {
        const QString pattern = QLatin1Char('%') + likeEscape(token) + QLatin1Char('%');
        query.addBindValue(pattern);
        query.addBindValue(pattern);
    }
    query.addBindValue(limit);

    if (!query.exec())
        return results;

    while (query.next())
        results.append(readRecord(query));
    return results;
}

QVector<ExecutableRecord> Database::recentlyUsed(int limit) const
{
    QVector<ExecutableRecord> results;
    if (!isOpen())
        return results;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT ") + selectColumns() + selectFrom()
                  + QStringLiteral(" WHERE u.launch_count > 0"
                                    " ORDER BY u.last_used DESC LIMIT ?"));
    query.addBindValue(limit);
    if (!query.exec())
        return results;

    while (query.next())
        results.append(readRecord(query));
    return results;
}

QVector<ExecutableRecord> Database::recentlyModified(int limit) const
{
    QVector<ExecutableRecord> results;
    if (!isOpen())
        return results;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT ") + selectColumns() + selectFrom()
                  + QStringLiteral(" ORDER BY e.mtime DESC LIMIT ?"));
    query.addBindValue(limit);
    if (!query.exec())
        return results;

    while (query.next())
        results.append(readRecord(query));
    return results;
}

void Database::recordLaunch(const QString &path)
{
    if (!isOpen() || path.isEmpty())
        return;

    // 统一使用原生分隔符，保证与 executables.path 的 join 键一致
    const QString normalizedPath = QDir::toNativeSeparators(path);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery update(*m_db);
    update.prepare(QStringLiteral("UPDATE usage_stats"
                                  " SET launch_count = launch_count + 1, last_used = ?"
                                  " WHERE path = ?"));
    update.addBindValue(now);
    update.addBindValue(normalizedPath);

    if (!update.exec() || update.numRowsAffected() == 0) {
        QSqlQuery insert(*m_db);
        insert.prepare(QStringLiteral("INSERT OR IGNORE INTO usage_stats (path, launch_count, last_used)"
                                      " VALUES (?, 1, ?)"));
        insert.addBindValue(normalizedPath);
        insert.addBindValue(now);
        insert.exec();
    }
}

QString Database::meta(const QString &key, const QString &fallback) const
{
    if (!isOpen())
        return fallback;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT value FROM app_meta WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next())
        return fallback;
    return query.value(0).toString();
}

void Database::setMeta(const QString &key, const QString &value)
{
    if (!isOpen())
        return;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO app_meta (key, value) VALUES (?, ?)"));
    query.addBindValue(key);
    query.addBindValue(value);
    query.exec();
}

QString Database::likeEscape(const QString &value) const
{
    QString escaped;
    escaped.reserve(value.size());
    for (const QChar ch : value) {
        if (ch == QLatin1Char('%') || ch == QLatin1Char('_') || ch == QLatin1Char('\\'))
            escaped.append(QLatin1Char('\\'));
        escaped.append(ch);
    }
    return escaped;
}
