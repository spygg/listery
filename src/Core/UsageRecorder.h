#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QString>

/**
 * 使用统计写入器：在独立后台线程里维护自己的 SQLite 连接，
 * 把“记录启动次数 / 最近使用时间”这类写操作从 GUI 线程挪开。
 *
 * 主线程通过 QueuedConnection 把 record() 投递到后台线程执行，
 * 因此即使扫描线程正持有索引库写锁，GUI 也不会被 recordLaunch 卡住。
 */
class UsageRecorder : public QObject
{
    Q_OBJECT
public:
    explicit UsageRecorder(const QString &databasePath, QObject *parent = nullptr);
    ~UsageRecorder() override;

public slots:
    void record(const QString &path);
    void shutdown();

private:
    bool ensureOpen();

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_db;
};
