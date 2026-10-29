#include "Core/SearchService.h"

#include "Core/Database.h"
#include "Core/QtCompat.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QVariant>

#include <memory>

/** 在检索线程里持有独立的数据库连接与检索引擎。 */
class SearchWorker : public QObject
{
    Q_OBJECT

public:
    explicit SearchWorker(const QString &databasePath)
        : m_databasePath(databasePath)
    {
    }

    bool initialize(QString *errorMessage)
    {
        m_database.reset(new Database(m_databasePath));
        if (!m_database->open(errorMessage)) {
            m_database.reset();
            return false;
        }
        m_engine.reset(new SearchEngine(m_database.get()));
        return true;
    }

public slots:
    void requestSearch(const QString &query, int limit, quint64 sequence)
    {
        m_pendingQuery = query;
        m_pendingLimit = limit;
        m_pendingSequence = sequence;
        // 同一次事件循环里排队的多次输入只保留最后一次，避免一长串过期查询排队
        if (m_scheduled)
            return;
        m_scheduled = true;
        QMetaObject::invokeMethod(this, "runPending", Qt::QueuedConnection);
    }

    void runPending()
    {
        m_scheduled = false;
        if (!m_engine || m_pendingSequence <= m_doneSequence)
            return;

        const quint64 sequence = m_pendingSequence;
        const QString query = m_pendingQuery;
        const int limit = m_pendingLimit;

        QElapsedTimer timer;
        timer.start();
        const QVector<SearchResult> results = m_engine->search(query, limit);
        m_doneSequence = sequence;
        emit resultsReady(sequence, query, results, timer.elapsed());
    }

    void shutdown()
    {
        m_engine.reset();
        m_database.reset();
    }

signals:
    void resultsReady(quint64 sequence, const QString &query,
                      const QVector<SearchResult> &results, qint64 elapsedMs);

private:
    QString m_databasePath;
    std::unique_ptr<Database> m_database;
    std::unique_ptr<SearchEngine> m_engine;
    QString m_pendingQuery;
    int m_pendingLimit = 0;
    quint64 m_pendingSequence = 0;
    quint64 m_doneSequence = 0;
    bool m_scheduled = false;
};

SearchService::SearchService(const QString &databasePath, QObject *parent)
    : QObject(parent)
    , m_databasePath(databasePath)
{
    qRegisterMetaType<quint64>("quint64");
    qRegisterMetaType<QVector<SearchResult>>("QVector<SearchResult>");
}

SearchService::~SearchService()
{
    stop();
}

bool SearchService::start(QString *errorMessage)
{
    if (m_thread)
        return true;

    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("listery-search"));
    m_worker = new SearchWorker(m_databasePath);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &SearchWorker::shutdown);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &SearchWorker::resultsReady, this, &SearchService::resultsReady,
            Qt::QueuedConnection);
    m_thread->start();

    bool ok = false;
    QString error;
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    QMetaObject::invokeMethod(m_worker, [this, &ok, &error]() { ok = m_worker->initialize(&error); },
                              Qt::BlockingQueuedConnection);
#else
    // Qt 5.7 没有 functor 版 invokeMethod：用 QTimer::singleShot 投递到工作线程，
    // 再用事件循环阻塞等待结果（语义等价 BlockingQueuedConnection）
    QEventLoop loop;
    QTimer::singleShot(0, m_worker, [this, &ok, &error, &loop]() {
        ok = m_worker->initialize(&error);
        loop.quit();
    });
    loop.exec();
#endif
    if (!ok) {
        stop();
        if (errorMessage)
            *errorMessage = error;
        return false;
    }
    return true;
}

void SearchService::stop()
{
    if (!m_thread)
        return;

    m_thread->quit();
    if (!m_thread->wait(3000)) {
        // 宁可漏掉一个 QThread 对象也不能删还在跑的对象（析构会直接终止进程），
        // 更不能 terminate 正在写 SQLite 的线程
        qWarning("Listery: 检索线程未能在 3 秒内退出，交由进程退出时回收");
        m_thread->setParent(nullptr);
        m_thread = nullptr;
        m_worker = nullptr;
        return;
    }
    delete m_thread;
    m_thread = nullptr;
    m_worker = nullptr;
}

quint64 SearchService::search(const QString &query, int limit)
{
    const quint64 sequence = ++m_sequence;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "requestSearch", Qt::QueuedConnection,
                                  Q_ARG(QString, query), Q_ARG(int, limit),
                                  Q_ARG(quint64, sequence));
    }
    return sequence;
}

#include "SearchService.moc"
