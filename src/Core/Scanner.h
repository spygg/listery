#pragma once

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QThread>

class Database;
struct VolumeStat;

/**
 * 全盘扫描线程。
 *
 * 扫描策略：
 *  - 逐个卷递归遍历，命中后缀白名单的文件写入 SQLite；
 *  - 遍历用 QDirIterator，先把名字过滤掉再 stat，绝大多数无关文件不产生系统调用；
 *  - Linux 上优先读 locate/mlocate/plocate 的快照（秒级完成），只有它不可用或需要复核时才真遍历；
 *  - 跳过系统缓存目录（WinSxS、SoftwareDistribution 等），避免无谓 IO；
 *  - 每 2000 条提交一次事务，兼顾速度与内存；
 *  - 本轮记录打上 generation 标记，整轮跑完后删除没再见到的旧记录（只删本次扫到的卷）；
 *  - 每个卷单独统计条目数，供调度器决定下一轮何时再扫它；
 *  - 线程以最低优先级运行，不与前台抢 CPU。
 */
class Scanner : public QThread
{
    Q_OBJECT

public:
    Scanner(const QString &databasePath,
            const QStringList &extensions,
            const QStringList &skipPatterns,
            bool includeRemovable,
            qint64 generation,
            /** 要扫描的卷；为空表示扫描所有本地卷。只扫部分卷时不会清理其它卷的记录。 */
            const QStringList &volumes = QStringList(),
            /** true = 强制全量遍历（不使用 locate 快照，用于人工重建索引与可疑结果复核）。 */
            bool deep = false,
            QObject *parent = nullptr);
    ~Scanner() override;

    void requestStop();
    bool isStopping() const;

    static QStringList availableRoots(bool includeRemovable);
    /** 当前用户的 AppData\Local 与 AppData\Roaming（仅 Windows 有意义，其余平台返回空）。
     *  大量用户级应用（豆包、Discord、VS Code 等）装在这里，而这些目录是隐藏目录，
     *  全盘遍历进不去，所以作为显式扫描根单独扫。 */
    static QStringList userAppDataRoots();
    /** 这一轮要扫的卷里，有哪些隐藏根（开始菜单、AppData）需要一并扫。
     *  隐藏根位于卷之内，但它本身是隐藏目录，全盘遍历进不去；定时重扫只扫到期的卷，
     *  若不同时扫这些隐藏根，它们拿不到当前 generation，会被 removeStale 误删。 */
    static QStringList hiddenRootsFor(const QStringList &volumes);
    /** locate 是否可用（仅 Unix）：用于向用户解释为什么这一轮这么快。 */
    static bool locateAvailable();

signals:
    void progressChanged(const QString &root, int directories, int filesFound);
    void scanFinished(int filesFound, int removed, bool canceled, const QString &error);

protected:
    void run() override;

private:
    /** depth 是当前目录层级（从 0 起），maxDepth 为允许下钻的最大层级，超过即停止递归。
     *  对开始菜单 / AppData 这类隐藏根使用较小的 maxDepth，避免掉进浏览器的深层缓存树里。 */
    void scanDirectory(const QString &path, int depth, Database &db, int maxDepth = 64);
    bool isSkippedDirectory(const QString &name) const;
    bool isSkippedPath(const QString &path) const;
    void emitProgress(const QString &root, bool force = false);
    /** 收下一份候选文件：白名单后缀命中，或（Unix 快速通道）带可执行位。 */
    bool ingestFile(const QFileInfo &info, Database &db, bool allowExtensionless);

    // Linux 快速通道：直接读 locate 快照
    bool scanViaLocate(const QStringList &roots, Database &db);
    static QString locateProgram();
    static qint64 locateDatabaseMtime();
    static QString extensionPattern(const QStringList &extensions);
    void recordVolumeStat(const QString &root, Database &db);

    QString m_databasePath;
    QStringList m_extensions;
    QSet<QString> m_extensionSet;
    QStringList m_skipPatterns;
    /** m_skipPatterns 的小写副本：每层目录都要比对一遍，现算 toLower 是白扔分配。 */
    QStringList m_skipPatternsLower;
    bool m_includeRemovable = true;
    qint64 m_generation = 0;
    QStringList m_volumes;
    bool m_deep = false;

    QAtomicInt m_stop;
    int m_directories = 0;
    int m_filesFound = 0;
    int m_pendingWrites = 0;
    QString m_currentRoot;
    QElapsedTimer m_progressTimer;
    QString m_error;
    /** 本轮是否走了 locate 快照；为 true 时清理旧记录要保护比快照更新的文件。 */
    bool m_usedLocate = false;
    qint64 m_locateDbMtime = 0;
};
