#pragma once

#include <QHash>
#include <QMetaType>
#include <QString>
#include <QVector>

#include <memory>

class QSqlDatabase;
class QSqlQuery;

/** 一条可执行文件索引记录。 */
struct ExecutableRecord
{
    QString path;        // 完整路径（原生分隔符）
    QString displayName; // 原始大小写的文件名
    QString dir;         // 所在目录（小写，便于匹配）
    QString ext;         // 扩展名（小写，不含点）
    QString pinyin;      // 汉字名的全拼变体（空格分隔），非中文名为空
    QString pinyinInitials; // 与 pinyin 一一对应的首字母变体
    qint64 size = 0;
    qint64 mtime = 0;
    int launchCount = 0;
    qint64 lastUsed = 0; // 毫秒时间戳
};

Q_DECLARE_METATYPE(ExecutableRecord)

/** 使用统计快照：整表很小，每次检索读一遍比逐行 JOIN 便宜。 */
struct UsageStat
{
    int launchCount = 0;
    qint64 lastUsed = 0;
};

/**
 * SQLite 索引库。
 *
 * 设计要点：
 *  - 可执行文件表与使用统计表分离，重建索引时不会丢失使用频次；
 *  - 每轮扫描使用 generation 标记，扫描结束后删除本轮没见到的旧记录；
 *  - 每个线程使用独立连接（QSqlDatabase 不能跨线程共享），WAL 模式保证读写并发。
 */
class Database
{
public:
    explicit Database(const QString &databasePath);
    ~Database();

    bool open(QString *errorMessage = nullptr);
    void close();
    bool isOpen() const;

    // ----- 扫描端 -----
    bool beginBulk();
    bool upsertExecutable(const ExecutableRecord &record, qint64 generation);
    bool commitBulk();
    bool rollbackBulk();
    int removeStale(qint64 generation, QString *errorMessage = nullptr);
    int totalCount() const;
    bool clearAll();

    // ----- 查询端 -----
    bool ftsAvailable() const;
    QVector<ExecutableRecord> searchByFts(const QStringList &tokens, int limit, bool nameOnly) const;
    QVector<ExecutableRecord> searchByTokens(const QStringList &tokens, int limit) const;
    QVector<ExecutableRecord> searchByPrefix(const QString &prefix, int limit) const;
    QVector<ExecutableRecord> searchByExactName(const QString &name, int limit) const;
    /** 已启动过的程序里，名字匹配这些关键字前缀的条目，按使用强排序。 */
    QVector<ExecutableRecord> searchUsageSeeds(const QStringList &tokens, int limit) const;
    QHash<QString, UsageStat> usageSnapshot() const;
    QVector<ExecutableRecord> recentlyUsed(int limit) const;
    QVector<ExecutableRecord> recentlyModified(int limit) const;
    void recordLaunch(const QString &path);

    // ----- 元信息 -----
    QString meta(const QString &key, const QString &fallback = QString()) const;
    void setMeta(const QString &key, const QString &value);

private:
    bool createSchema(QString *errorMessage);
    bool setupFts(QString *errorMessage);
    bool ensureFtsIndex(QString *errorMessage);
    QString likeEscape(const QString &value) const;

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase *m_db = nullptr;
    std::unique_ptr<QSqlQuery> m_upsertQuery;
    QString m_lastError;
    bool m_ftsAvailable = false;
};
