#pragma once

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>

class Database;

/**
 * 全盘扫描线程。
 *
 * 扫描策略：
 *  - 逐个卷递归遍历，命中后缀白名单的文件写入 SQLite；
 *  - 跳过系统缓存目录（WinSxS、SoftwareDistribution 等），避免无谓 IO；
 *  - 每 2000 条提交一次事务，兼顾速度与内存；
 *  - 本轮记录打上 generation 标记，整轮跑完后删除没再见到的旧记录。
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
            const QStringList &rootsOverride = QStringList(),
            QObject *parent = nullptr);
    ~Scanner() override;

    void requestStop();
    bool isStopping() const;

    static QStringList availableRoots(bool includeRemovable);

signals:
    void progressChanged(const QString &root, int directories, int filesFound);
    void scanFinished(int filesFound, int removed, bool canceled, const QString &error);

protected:
    void run() override;

private:
    void scanDirectory(const QString &path, int depth, Database &db);
    bool isSkippedDirectory(const QString &name) const;
    void emitProgress(const QString &root, bool force = false);

    QString m_databasePath;
    QStringList m_extensions;
    QSet<QString> m_extensionSet;
    QStringList m_skipPatterns;
    bool m_includeRemovable = true;
    qint64 m_generation = 0;
    /** 仅自检/测试用于限定目录；正常运行保持为空，表示扫描所有本地磁盘。 */
    QStringList m_rootsOverride;

    QAtomicInt m_stop;
    int m_directories = 0;
    int m_filesFound = 0;
    int m_pendingWrites = 0;
    QString m_currentRoot;
    QElapsedTimer m_progressTimer;
    QString m_error;
};
