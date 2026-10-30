#include "Core/Database.h"

#include "Core/QtCompat.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVariant>
#include <QVariantList>

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

/** 粗筛只取匹配需要的列，使用统计随后由 SearchEngine 用快照补齐。 */
QString lightColumns()
{
    return QStringLiteral("e.path, e.display_name, e.dir, e.ext, e.pinyin, e.pinyin_initials");
}

QString lightFrom()
{
    return QStringLiteral(" FROM executables e");
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

ExecutableRecord readLightRecord(const QSqlQuery &query)
{
    ExecutableRecord record;
    record.path = query.value(0).toString();
    record.displayName = query.value(1).toString();
    record.dir = query.value(2).toString();
    record.ext = query.value(3).toString();
    record.pinyin = query.value(4).toString();
    record.pinyinInitials = query.value(5).toString();
    return record;
}

bool isAsciiWord(const QString &term)
{
    for (const QChar ch : term) {
        if (ch.unicode() >= 128 || !ch.isLetterOrNumber())
            return false;
    }
    return true;
}

/**
 * 卷根路径 → 路径前缀。QStorageInfo 给的是 "C:/"、"/home"，
 * 而库里存的是原生分隔符的绝对路径，比较前统一成 ".../"/ "...\" 结尾的前缀。
 */
QString volumePrefix(const QString &root)
{
    if (root.isEmpty())
        return root;

    QString prefix = QDir::toNativeSeparators(root);
    const QChar last = prefix.at(prefix.size() - 1);
    if (last != QLatin1Char('/') && last != QLatin1Char('\\'))
        prefix.append(QDir::toNativeSeparators(QStringLiteral("/")));
    return prefix;
}

bool isCommonExtensionTerm(const QString &term)
{
    static const QStringList kTerms = {
        QStringLiteral("exe"), QStringLiteral("com"), QStringLiteral("msi"),
        QStringLiteral("scr"), QStringLiteral("sys"), QStringLiteral("dll"),
    };
    return kTerms.contains(term);
}

/** FTS5 分词器按标点切词，带点号/下划线的整段短语在 detail=column 下会直接报错。 */
QStringList ftsTerms(const QString &token)
{
    QStringList terms;
    QString current;
    for (const QChar ch : token) {
        if (ch.isLetterOrNumber()) {
            current.append(ch);
        } else if (!current.isEmpty()) {
            terms.append(current);
            current.clear();
        }
    }
    if (!current.isEmpty())
        terms.append(current);
    return terms;
}
} // namespace

Database::Database(const QString &databasePath)
    : m_databasePath(databasePath)
    , m_connectionName(QStringLiteral("listery_%1").arg(listeryUuidString()))
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
        // 失败时也要释放连接：否则再次 open() 会用同名连接再 addDatabase 一次，
        // 旧连接被 Qt 移除、指针悬垂，还会一直占着数据库句柄
        close();
        return false;
    }

    QSqlQuery pragma(*m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
    pragma.exec(QStringLiteral("PRAGMA temp_store = MEMORY"));
    pragma.exec(QStringLiteral("PRAGMA cache_size = -20000"));
    // 注意：不要再用 PRAGMA case_sensitive_like —— SQLite 已把它标记为弃用（未来版本会移除）。
    // 前缀查询靠索引上的 COLLATE NOCASE 走范围扫描（见 createSchema），语义与默认 LIKE 一致。

    return createSchema(errorMessage);
}

bool Database::createSchema(QString *errorMessage)
{
    QSqlQuery query(*m_db);

    // 索引迁移：老版本用 PRAGMA case_sensitive_like 让前缀 LIKE 走 BINARY 索引，该 pragma 已弃用。
    // 现在改为在索引上显式声明 COLLATE NOCASE（默认 LIKE 本来就不区分大小写），
    // 老库里的同名 BINARY 索引必须删掉重建，否则前缀查询会退化成全表扫描。
    int userVersion = 0;
    {
        QSqlQuery versionQuery(*m_db);
        if (versionQuery.exec(QStringLiteral("PRAGMA user_version")) && versionQuery.next())
            userVersion = versionQuery.value(0).toInt();
    }
    if (userVersion < 2) {
        static const char *kPrefixIndexes[] = {"idx_exe_name", "idx_exe_pinyin",
                                               "idx_exe_pinyin_initials"};
        for (const char *index : kPrefixIndexes) {
            if (!query.exec(QStringLiteral("DROP INDEX IF EXISTS %1").arg(QString::fromLatin1(index)))) {
                m_lastError = query.lastError().text();
                if (errorMessage)
                    *errorMessage = m_lastError;
                return false;
            }
        }
    }

    const QStringList statements = QStringList()
        << QStringLiteral(
               "CREATE TABLE IF NOT EXISTS executables ("
               " path TEXT NOT NULL PRIMARY KEY,"
               " name TEXT NOT NULL,"
               " display_name TEXT NOT NULL,"
               " dir TEXT NOT NULL,"
               " ext TEXT NOT NULL,"
               " pinyin TEXT NOT NULL DEFAULT '',"
               " pinyin_initials TEXT NOT NULL DEFAULT '',"
               " size INTEGER NOT NULL DEFAULT 0,"
               " mtime INTEGER NOT NULL DEFAULT 0,"
               " drive TEXT NOT NULL DEFAULT '',"
               " gen INTEGER NOT NULL DEFAULT 0"
               ")")
        // 这三个索引供 LIKE 'x%' 前缀查询使用：COLLATE NOCASE 让默认（大小写不敏感）的
        // LIKE 也能走范围扫描，不再依赖已弃用的 case_sensitive_like
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_name ON executables(name COLLATE NOCASE)")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_dir ON executables(dir)")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_gen ON executables(gen)")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_mtime ON executables(mtime)")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_pinyin ON executables(pinyin COLLATE NOCASE)")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_exe_pinyin_initials"
                          " ON executables(pinyin_initials COLLATE NOCASE)")
        << QStringLiteral(
               "CREATE TABLE IF NOT EXISTS usage_stats ("
               " path TEXT NOT NULL PRIMARY KEY,"
               " launch_count INTEGER NOT NULL DEFAULT 0,"
               " last_used INTEGER NOT NULL DEFAULT 0"
               ")")
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_usage_last ON usage_stats(last_used DESC)")
        // 使用率种子每敲一个键都要按频次取前几百条，没有这个索引就得整表排序
        << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_usage_count ON usage_stats(launch_count DESC)")
        << QStringLiteral(
               "CREATE TABLE IF NOT EXISTS app_meta ("
               " key TEXT NOT NULL PRIMARY KEY,"
               " value TEXT NOT NULL DEFAULT ''"
               ")")
        // 每个磁盘卷的索引概况：智能调度据此决定这个卷什么时候值得再扫
        << QStringLiteral(
               "CREATE TABLE IF NOT EXISTS volume_stats ("
               " root TEXT NOT NULL PRIMARY KEY,"
               " exe_count INTEGER NOT NULL DEFAULT 0,"
               " last_scan INTEGER NOT NULL DEFAULT 0,"
               " empty_rounds INTEGER NOT NULL DEFAULT 0,"
               " deep_pending INTEGER NOT NULL DEFAULT 0"
               ")");

    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            if (errorMessage)
                *errorMessage = m_lastError;
            return false;
        }
    }

    // 老库补列：CREATE TABLE IF NOT EXISTS 不会给已存在的表加字段
    QStringList existing;
    if (query.exec(QStringLiteral("PRAGMA table_info(executables)"))) {
        while (query.next())
            existing.append(query.value(1).toString());
    }
    const QStringList addedColumns = QStringList()
        << QStringLiteral("pinyin") << QStringLiteral("pinyin_initials");
    for (const QString &column : addedColumns) {
        if (existing.contains(column))
            continue;
        if (!query.exec(QStringLiteral("ALTER TABLE executables ADD COLUMN %1 TEXT NOT NULL DEFAULT ''")
                            .arg(column))) {
            m_lastError = query.lastError().text();
            if (errorMessage)
                *errorMessage = m_lastError;
            return false;
        }
    }

    // FTS5 只是加速手段：老版本 SQLite 没有 FTS5 时静默退化为 LIKE 全表扫描
    setupFts(errorMessage);
    return true;
}

bool Database::setupFts(QString *errorMessage)
{
    QSqlQuery query(*m_db);
    // 索引列随版本变化（v2 起多了拼音两列）：版本不一致就整表重建，旧版本没有拼音数据
    if (meta(QStringLiteral("fts_version")) != QStringLiteral("2")) {
        query.exec(QStringLiteral("DROP TRIGGER IF EXISTS exe_fts_ai"));
        query.exec(QStringLiteral("DROP TRIGGER IF EXISTS exe_fts_ad"));
        query.exec(QStringLiteral("DROP TRIGGER IF EXISTS exe_fts_au"));
        query.exec(QStringLiteral("DROP TABLE IF EXISTS exe_fts"));
    }

    // detail=column 不存词位置（我们只做词首前缀匹配，不需要短语查询），索引体积约为完整模式的一半
    if (!query.exec(QStringLiteral(
            "CREATE VIRTUAL TABLE IF NOT EXISTS exe_fts USING fts5("
            " name, dir, pinyin, pinyin_initials,"
            " content = 'executables', content_rowid = 'rowid', detail = column)"))) {
        qWarning("Listery: 当前 SQLite 不支持 FTS5，检索退化为 LIKE 扫描：%s",
                 qPrintable(query.lastError().text()));
        m_ftsAvailable = false;
        return false;
    }

    const QStringList triggers = QStringList()
        << QStringLiteral(
               "CREATE TRIGGER IF NOT EXISTS exe_fts_ai AFTER INSERT ON executables BEGIN"
               " INSERT INTO exe_fts(rowid, name, dir, pinyin, pinyin_initials)"
               " VALUES (new.rowid, new.name, new.dir, new.pinyin, new.pinyin_initials);"
               " END")
        << QStringLiteral(
               "CREATE TRIGGER IF NOT EXISTS exe_fts_ad AFTER DELETE ON executables BEGIN"
               " INSERT INTO exe_fts(exe_fts, rowid, name, dir, pinyin, pinyin_initials)"
               " VALUES ('delete', old.rowid, old.name, old.dir, old.pinyin, old.pinyin_initials);"
               " END")
        // 扫描时每行都会走 UPDATE，只有检索列真的变了才重建索引行
        << QStringLiteral(
               "CREATE TRIGGER IF NOT EXISTS exe_fts_au AFTER UPDATE ON executables"
               " WHEN old.name <> new.name OR old.dir <> new.dir"
               " OR old.pinyin <> new.pinyin OR old.pinyin_initials <> new.pinyin_initials BEGIN"
               " INSERT INTO exe_fts(exe_fts, rowid, name, dir, pinyin, pinyin_initials)"
               " VALUES ('delete', old.rowid, old.name, old.dir, old.pinyin, old.pinyin_initials);"
               " INSERT INTO exe_fts(rowid, name, dir, pinyin, pinyin_initials)"
               " VALUES (new.rowid, new.name, new.dir, new.pinyin, new.pinyin_initials);"
               " END");

    for (const QString &trigger : triggers) {
        if (!query.exec(trigger)) {
            qWarning("Listery: 创建 FTS 同步触发器失败：%s", qPrintable(query.lastError().text()));
            m_ftsAvailable = false;
            return false;
        }
    }

    m_ftsAvailable = true;
    if (!ensureFtsIndex(errorMessage)) {
        qWarning("Listery: FTS 索引初始化失败，本次退化为 LIKE 扫描：%s",
                 qPrintable(m_lastError));
        m_ftsAvailable = false;
        return false;
    }
    return true;
}

bool Database::ensureFtsIndex(QString *errorMessage)
{
    // 常见路径只读元信息；首次升级时才拿写锁重建，避免阻塞扫描事务
    if (meta(QStringLiteral("fts_version")) == QStringLiteral("2"))
        return true;

    QSqlQuery transaction(*m_db);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        m_lastError = transaction.lastError().text();
        if (errorMessage)
            *errorMessage = m_lastError;
        return false;
    }

    bool ok = true;
    if (meta(QStringLiteral("fts_version")) != QStringLiteral("2")) {
        QSqlQuery rebuild(*m_db);
        ok = rebuild.exec(QStringLiteral("INSERT INTO exe_fts(exe_fts) VALUES('rebuild')"));
        if (ok) {
            setMeta(QStringLiteral("fts_version"), QStringLiteral("2"));
        } else {
            m_lastError = rebuild.lastError().text();
            if (errorMessage)
                *errorMessage = m_lastError;
        }
    }

    transaction.exec(ok ? QStringLiteral("COMMIT") : QStringLiteral("ROLLBACK"));
    return ok;
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
        // 用 ON CONFLICT 更新而不是 INSERT OR REPLACE，保持 rowid 稳定（FTS/排序都依赖它）
        m_upsertQuery->prepare(
            QStringLiteral("INSERT INTO executables"
                           " (path, name, display_name, dir, ext, pinyin, pinyin_initials,"
                           "  size, mtime, drive, gen)"
                           " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
                           " ON CONFLICT(path) DO UPDATE SET"
                           " name = excluded.name,"
                           " display_name = excluded.display_name,"
                           " dir = excluded.dir,"
                           " ext = excluded.ext,"
                           " pinyin = excluded.pinyin,"
                           " pinyin_initials = excluded.pinyin_initials,"
                           " size = excluded.size,"
                           " mtime = excluded.mtime,"
                           " drive = excluded.drive,"
                           " gen = excluded.gen"));
    }
    return m_db->transaction();
}

bool Database::upsertExecutable(const ExecutableRecord &record, qint64 generation)
{
    if (!m_upsertQuery)
        return false;

    const QString normalizedPath = QDir::toNativeSeparators(record.path);
    m_upsertQuery->bindValue(0, normalizedPath);
    m_upsertQuery->bindValue(1, record.displayName.toLower());
    m_upsertQuery->bindValue(2, record.displayName);
    m_upsertQuery->bindValue(3, record.dir.toLower());
    m_upsertQuery->bindValue(4, record.ext.toLower());
    // QString() 是 null，直接绑会触发 pinyin 列的 NOT NULL 约束
    m_upsertQuery->bindValue(5, record.pinyin.isNull() ? QStringLiteral("") : record.pinyin);
    m_upsertQuery->bindValue(6, record.pinyinInitials.isNull() ? QStringLiteral("")
                                                               : record.pinyinInitials);
    m_upsertQuery->bindValue(7, record.size);
    m_upsertQuery->bindValue(8, record.mtime);
    m_upsertQuery->bindValue(9, normalizedPath.left(2).toUpper());
    m_upsertQuery->bindValue(10, generation);
    const bool ok = m_upsertQuery->exec();
    if (!ok)
        qWarning("Listery: 写入索引失败：%s", qPrintable(m_upsertQuery->lastError().text()));
    return ok;
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
    return removeStale(generation, QStringList(), 0, errorMessage);
}

int Database::removeStale(qint64 generation, const QStringList &volumes, qint64 keepNewerThan,
                          QString *errorMessage)
{
    if (!isOpen())
        return 0;

    QStringList conditions;
    QVariantList binds;
    conditions.append(QStringLiteral("gen <> ?"));
    binds.append(QVariant::fromValue(generation));

    if (!volumes.isEmpty()) {
        QStringList volumeClauses;
        for (const QString &volume : volumes) {
            const QString prefix = volumePrefix(volume);
            if (prefix.isEmpty())
                continue;
            // 这里保留 LIKE：path 是 BINARY 排序，而默认 LIKE 大小写不敏感，
            // 用不了索引（会退化成覆盖索引全扫）；本查询每轮扫描只跑一次，可接受
            volumeClauses.append(QStringLiteral("path LIKE ? ESCAPE '\\'"));
            binds.append(likeEscape(prefix) + QLatin1Char('%'));
        }
        if (volumeClauses.isEmpty())
            return 0;
        conditions.append(QStringLiteral("(%1)").arg(volumeClauses.join(QStringLiteral(" OR "))));
    }

    if (keepNewerThan > 0) {
        conditions.append(QStringLiteral("mtime < ?"));
        binds.append(QVariant::fromValue(keepNewerThan));
    }

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("DELETE FROM executables WHERE ")
                  + conditions.join(QStringLiteral(" AND ")));
    for (const QVariant &bind : binds)
        query.addBindValue(bind);

    if (!query.exec()) {
        if (errorMessage)
            *errorMessage = query.lastError().text();
        return -1;
    }
    return query.numRowsAffected();
}

int Database::pruneOrphanUsage()
{
    if (!isOpen())
        return 0;

    QSqlQuery query(*m_db);
    // 索引清空时不要连带把使用历史全抹掉（那多半是库损坏后重建，统计本身还有价值）
    if (totalCount() == 0)
        return 0;
    // NOT EXISTS 走 executables.path 主键逐行探测；NOT IN 子查询会先物化整张表再比较，
    // 二十万条索引时慢一个数量级
    if (!query.exec(QStringLiteral(
            "DELETE FROM usage_stats WHERE NOT EXISTS"
            " (SELECT 1 FROM executables e WHERE e.path = usage_stats.path)"))) {
        return 0;
    }
    return query.numRowsAffected();
}

VolumeStat Database::volumeStat(const QString &root) const
{
    VolumeStat stat;
    stat.root = root;
    if (!isOpen() || root.isEmpty())
        return stat;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT exe_count, last_scan, empty_rounds, deep_pending"
                                 " FROM volume_stats WHERE root = ?"));
    query.addBindValue(root);
    if (!query.exec() || !query.next())
        return stat;

    stat.exeCount = query.value(0).toInt();
    stat.lastScan = query.value(1).toLongLong();
    stat.emptyRounds = query.value(2).toInt();
    stat.deepPending = query.value(3).toInt() != 0;
    return stat;
}

void Database::setVolumeStat(const VolumeStat &stat)
{
    if (!isOpen() || stat.root.isEmpty())
        return;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO volume_stats"
                                 " (root, exe_count, last_scan, empty_rounds, deep_pending)"
                                 " VALUES (?, ?, ?, ?, ?)"));
    query.addBindValue(stat.root);
    query.addBindValue(stat.exeCount);
    query.addBindValue(QVariant::fromValue(stat.lastScan));
    query.addBindValue(stat.emptyRounds);
    query.addBindValue(stat.deepPending ? 1 : 0);
    query.exec();
}

int Database::countInVolume(const QString &root) const
{
    if (!isOpen())
        return 0;

    const QString prefix = volumePrefix(root);
    if (prefix.isEmpty())
        return 0;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM executables WHERE path LIKE ? ESCAPE '\\'"));
    query.addBindValue(likeEscape(prefix) + QLatin1Char('%'));
    if (!query.exec() || !query.next())
        return 0;
    return query.value(0).toInt();
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

bool Database::ftsAvailable() const
{
    return m_ftsAvailable && isOpen();
}

QVector<ExecutableRecord> Database::searchByFts(const QStringList &tokens, int limit, bool nameOnly) const
{
    QVector<ExecutableRecord> results;
    if (!isOpen() || !m_ftsAvailable || tokens.isEmpty() || limit <= 0)
        return results;

    QStringList terms;
    for (const QString &token : tokens)
        terms.append(ftsTerms(token));
    // “exe” 出现在八成文件名里，与它求交比逐个取行还慢（实测 5ms 只为 1 条结果）；
    // 多词查询时去掉扩展名词根不影响召回，扩展名的精确匹配由内存重排负责
    if (terms.size() > 1) {
        QStringList filtered;
        for (const QString &term : terms) {
            if (!isCommonExtensionTerm(term))
                filtered.append(term);
        }
        if (!filtered.isEmpty())
            terms = filtered;
    }

    QStringList clauses;
    for (const QString &term : terms) {
        const QString phrase = QStringLiteral("\"%1\"*").arg(term);
        if (nameOnly) {
            clauses.append(QStringLiteral("name : %1").arg(phrase));
            continue;
        }

        // 拼音列与名称/目录必须并列成 OR：纯英文名的拼音列是空串，
        // 用 AND 拼会把这些行全部排除掉
        QStringList alternatives;
        alternatives << QStringLiteral("name : %1").arg(phrase)
                     << QStringLiteral("dir : %1").arg(phrase);
        // 单字母没有区分度，跳过以免首键把整本拼音表翻出来
        if (term.size() >= 2 && isAsciiWord(term))
            alternatives << QStringLiteral("pinyin : %1").arg(phrase)
                         << QStringLiteral("pinyin_initials : %1").arg(phrase);
        clauses.append(QStringLiteral("(%1)").arg(alternatives.join(QStringLiteral(" OR "))));
    }
    if (clauses.isEmpty())
        return results;

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT ") + lightColumns() + lightFrom()
                  + QStringLiteral(" WHERE e.rowid IN"
                                   " (SELECT rowid FROM exe_fts WHERE exe_fts MATCH ? LIMIT ?)"
                                   " LIMIT ?"));
    query.addBindValue(clauses.join(QStringLiteral(" AND ")));
    query.addBindValue(limit);
    query.addBindValue(limit);
    if (!query.exec()) {
        qWarning("Listery: FTS 查询失败（%s）：%s", qPrintable(clauses.join(QStringLiteral(" AND "))),
                 qPrintable(query.lastError().text()));
        return results;
    }

    while (query.next())
        results.append(readLightRecord(query));
    return results;
}

QVector<ExecutableRecord> Database::searchByPrefix(const QString &prefix, int limit) const
{
    QVector<ExecutableRecord> results;
    if (!isOpen() || prefix.isEmpty())
        return results;

    // 拼音列也要参与前缀匹配：输入 “wx” 需要能命中 “微信”（pinyin_initials = "wx ws"）。
    // 拼音列一律小写存储，与已转小写的 token 直接比前缀即可。
    // 保留 LIKE：pattern 是绑定参数、ESCAPE 用普通字符串字面量时，SQLite 仍能把前缀
    // 改写成 NOCASE 索引范围扫描（实测 40k 行 0.03ms），不需要手工算上下界。
    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT ") + lightColumns() + lightFrom()
                  + QStringLiteral(" WHERE (e.name LIKE ? ESCAPE '\\'"
                                  " OR e.pinyin_initials LIKE ? ESCAPE '\\'"
                                  " OR e.pinyin LIKE ? ESCAPE '\\') LIMIT ?"));
    const QString pattern = likeEscape(prefix) + QLatin1Char('%');
    query.addBindValue(pattern);
    query.addBindValue(pattern);
    query.addBindValue(pattern);
    query.addBindValue(limit);
    if (!query.exec())
        return results;

    while (query.next())
        results.append(readLightRecord(query));
    return results;
}

QVector<ExecutableRecord> Database::searchByExactName(const QString &name, int limit) const
{
    QVector<ExecutableRecord> results;
    if (!isOpen() || name.isEmpty() || limit <= 0)
        return results;

    // 必须显式 COLLATE NOCASE：列默认是 BINARY 排序，只写 name = ? 既走不了
    // idx_exe_name（每次按键全表扫 ~14ms），也会漏掉大小写不同的同名文件
    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT ") + lightColumns() + lightFrom()
                  + QStringLiteral(" WHERE e.name = ? COLLATE NOCASE LIMIT ?"));
    query.addBindValue(name);
    query.addBindValue(limit);
    if (!query.exec())
        return results;

    while (query.next())
        results.append(readLightRecord(query));
    return results;
}

QVector<ExecutableRecord> Database::searchUsageSeeds(const QStringList &tokens, int limit) const
{
    QVector<ExecutableRecord> results;
    if (!isOpen() || tokens.isEmpty() || limit <= 0)
        return results;

    QStringList clauses;
    for (int i = 0; i < tokens.size(); ++i) {
        clauses.append(QStringLiteral("(e.name LIKE ? ESCAPE '\\'"
                                      " OR e.pinyin LIKE ? ESCAPE '\\'"
                                      " OR e.pinyin_initials LIKE ? ESCAPE '\\')"));
    }

    // 用户真正启动过的程序通常只有几百个：先按使用强度取热榜，再按名字前缀过滤，
    // 这样被 rowid 顺序挤出候选区的常用程序仍然能参与排序
    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral(
                      "WITH hot AS (SELECT path, launch_count, last_used FROM usage_stats"
                      " WHERE launch_count > 0 ORDER BY launch_count DESC LIMIT 400)"
                      " SELECT ")
                  + lightColumns()
                  + QStringLiteral(" FROM hot u JOIN executables e ON e.path = u.path"
                                   " WHERE ")
                  + clauses.join(QStringLiteral(" AND "))
                  + QStringLiteral(" ORDER BY u.launch_count DESC, u.last_used DESC LIMIT ?"));
    for (const QString &token : tokens) {
        const QStringList parts = ftsTerms(token);
        const QString term = parts.isEmpty() ? token : parts.first();
        const QString pattern = likeEscape(term) + QLatin1Char('%');
        query.addBindValue(pattern);
        query.addBindValue(pattern);
        query.addBindValue(pattern);
    }
    query.addBindValue(limit);

    if (!query.exec())
        return results;

    while (query.next())
        results.append(readLightRecord(query));
    return results;
}

QHash<QString, UsageStat> Database::usageSnapshot() const
{
    QHash<QString, UsageStat> stats;
    if (!isOpen())
        return stats;

    QSqlQuery query(*m_db);
    if (!query.exec(QStringLiteral("SELECT path, launch_count, last_used FROM usage_stats")))
        return stats;

    stats.reserve(4096);
    while (query.next()) {
        UsageStat stat;
        stat.launchCount = query.value(1).toInt();
        stat.lastUsed = query.value(2).toLongLong();
        stats.insert(query.value(0).toString(), stat);
    }
    return stats;
}

QVector<ExecutableRecord> Database::searchByTokens(const QStringList &tokens, int limit) const
{
    QVector<ExecutableRecord> results;
    if (!isOpen() || tokens.isEmpty())
        return results;

    QStringList clauses;
    for (int i = 0; i < tokens.size(); ++i)
        clauses.append(QStringLiteral(
            "(e.name LIKE ? ESCAPE '\\' OR e.dir LIKE ? ESCAPE '\\'"
            " OR e.pinyin LIKE ? ESCAPE '\\' OR e.pinyin_initials LIKE ? ESCAPE '\\')"));

    QSqlQuery query(*m_db);
    query.prepare(QStringLiteral("SELECT ") + lightColumns() + lightFrom()
                  + QStringLiteral(" WHERE ") + clauses.join(QStringLiteral(" AND "))
                  + QStringLiteral(" LIMIT ?"));

    for (const QString &token : tokens) {
        const QString pattern = QLatin1Char('%') + likeEscape(token) + QLatin1Char('%');
        query.addBindValue(pattern);
        query.addBindValue(pattern);
        query.addBindValue(pattern);
        query.addBindValue(pattern);
    }
    query.addBindValue(limit);

    if (!query.exec())
        return results;

    while (query.next())
        results.append(readLightRecord(query));
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
