#pragma once

#include "Core/SearchEngine.h"

#include <QObject>
#include <QString>

class QThread;
class SearchWorker;

/**
 * 后台检索服务：SQLite 查询在独立线程执行，输入过程中 GUI 线程永不被检索阻塞。
 *
 * 请求带自增序号，worker 只执行最新的那次，排队中的过期请求直接跳过；
 * 各线程使用独立连接（QSqlDatabase 不能跨线程共享），WAL 保证与扫描互不阻塞。
 */
class SearchService : public QObject
{
    Q_OBJECT

public:
    explicit SearchService(const QString &databasePath, QObject *parent = nullptr);
    ~SearchService() override;

    bool start(QString *errorMessage = nullptr);
    void stop();

    /** 发起一次异步检索，返回本次请求序号（结果按序号回传，调用方可丢弃过期结果）。 */
    quint64 search(const QString &query, int limit);

signals:
    void resultsReady(quint64 sequence, const QString &query,
                      const QVector<SearchResult> &results, qint64 elapsedMs);

private:
    QString m_databasePath;
    QThread *m_thread = nullptr;
    SearchWorker *m_worker = nullptr;
    quint64 m_sequence = 0;
};
