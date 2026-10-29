#pragma once

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
    qint64 size = 0;
    qint64 mtime = 0;
    int launchCount = 0;
    qint64 lastUsed = 0; // 毫秒时间戳
};

Q_DECLARE_METATYPE(ExecutableRecord)

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
    QVector<ExecutableRecord> searchByTokens(const QStringList &tokens, int limit) const;
    QVector<ExecutableRecord> searchByPrefix(const QString &prefix, int limit) const;
    QVector<ExecutableRecord> recentlyUsed(int limit) const;
    QVector<ExecutableRecord> recentlyModified(int limit) const;
    void recordLaunch(const QString &path);

    // ----- 元信息 -----
    QString meta(const QString &key, const QString &fallback = QString()) const;
    void setMeta(const QString &key, const QString &value);

private:
    bool createSchema(QString *errorMessage);
    QString likeEscape(const QString &value) const;

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase *m_db = nullptr;
    std::unique_ptr<QSqlQuery> m_upsertQuery;
    QString m_lastError;
};
