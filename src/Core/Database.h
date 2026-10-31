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
    QString target;      // 快捷方式（.lnk）指向的目标路径，非快捷方式为空
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
 * 单个磁盘卷的索引概况，供“智能调度”决定下一次什么时候值得再扫它。
 *
 * 一个从没装过软件的分区（例如纯数据盘、备份盘）每次全盘遍历都要几分钟 IO，
 * 却一条结果都贡献不了；记录连续空扫的轮次后就可以把它的扫描频率指数级降下来。
 */
struct VolumeStat
{
    QString root;         // 卷根路径，例如 C:/、/home
    int exeCount = 0;     // 上次扫描结束时该卷的条目数
    qint64 lastScan = 0;  // 上次扫描完成时间（毫秒）
    int emptyRounds = 0;  // 连续“一条都没扫到”的轮次
    /** 快速通道（locate）结果可疑：下一轮必须用全量遍历复核。 */
    bool deepPending = false;
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
    /**
     * 删除本轮没再见到的旧记录。
     *
     * @param volumes        只清理这些卷（按路径前缀匹配）；为空表示清理整库。
     *                       只扫部分卷时必须传，否则会把没扫到的卷的记录一起删掉。
     * @param keepNewerThan  大于 0 时，保留 mtime 晚于该时间的记录。locate 快照早于文件系统
     *                       现状，刚装上、还没进 locate 库的程序不能被误删。
     */
    int removeStale(qint64 generation, const QStringList &volumes, qint64 keepNewerThan,
                    QString *errorMessage = nullptr);
    /** 删掉使用统计里已经不在索引中的孤儿记录。
     *  使用统计表没有外键约束，程序卸载后它的统计行会一直留着；而“常用程序先入候选”
     *  只取频次最高的前 400 条，孤儿行占住名额就会把真正在用的程序挤出候选区。 */
    int pruneOrphanUsage();
    int totalCount() const;
    bool clearAll();

    // ----- 卷级别统计（智能调度用） -----
    VolumeStat volumeStat(const QString &root) const;
    void setVolumeStat(const VolumeStat &stat);
    /** 该卷当前收录的条目数（按路径前缀统计）。 */
    int countInVolume(const QString &root) const;

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
