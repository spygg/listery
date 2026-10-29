#include "Core/UsageRecorder.h"

#include "Core/QtCompat.h"

#include <QDateTime>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QUuid>

UsageRecorder::UsageRecorder(const QString &databasePath, QObject *parent)
    : QObject(parent)
    , m_databasePath(databasePath)
    , m_connectionName(QStringLiteral("listery_usage_") + listeryUuidString())
{
}

UsageRecorder::~UsageRecorder()
{
    if (m_db.isOpen())
        m_db.close();
}

void UsageRecorder::shutdown()
{
    if (m_db.isOpen())
        m_db.close();
    // removeDatabase 要求没有其它句柄引用该连接，先放掉自己的副本
    m_db = QSqlDatabase();
    if (QSqlDatabase::contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool UsageRecorder::ensureOpen()
{
    if (m_db.isOpen())
        return true;

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_databasePath);
    if (!m_db.open()) {
        qWarning("Listery: 使用统计库打开失败：%s", qPrintable(m_db.lastError().text()));
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        return false;
    }

    // 使用统计是“尽力而为”的旁路写入，遇到扫描线程持锁时短等即放弃，不要拖累自己这条线程
    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA busy_timeout = 2000"));
    return true;
}

void UsageRecorder::record(const QString &path)
{
    if (path.isEmpty())
        return;
    if (!ensureOpen())
        return;

    // 统一使用原生分隔符，保证与 executables.path 的 join 键一致
    const QString normalizedPath = QDir::toNativeSeparators(path);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QSqlQuery update(m_db);
    update.prepare(QStringLiteral("UPDATE usage_stats"
                                  " SET launch_count = launch_count + 1, last_used = ?"
                                  " WHERE path = ?"));
    update.addBindValue(now);
    update.addBindValue(normalizedPath);

    if (!update.exec() || update.numRowsAffected() == 0) {
        QSqlQuery insert(m_db);
        insert.prepare(QStringLiteral("INSERT OR IGNORE INTO usage_stats (path, launch_count, last_used)"
                                      " VALUES (?, 1, ?)"));
        insert.addBindValue(normalizedPath);
        insert.addBindValue(now);
        if (!insert.exec())
            qWarning("Listery: 写入使用统计失败：%s", qPrintable(insert.lastError().text()));
    }
}
