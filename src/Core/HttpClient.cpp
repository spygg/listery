#include "Core/HttpClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTimer>

#include <functional>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winhttp.h>

#  include <string>
#  include <utility>
#else
#  include <utility>
#  include <QEventLoop>
#  include <QNetworkAccessManager>
#  include <QNetworkReply>
#  include <QNetworkRequest>
#  include <QSslSocket>
#  include <QTimer>
#endif

namespace {

/** 响应体上限：更新检查与插件查询都是小 JSON，超了多半是打到了错误页面。 */
const int kMaxBodyBytes = 8 * 1024 * 1024;

#ifdef Q_OS_WIN
QString winHttpErrorText(DWORD code)
{
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
        return QStringLiteral("网络请求超时");
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return QStringLiteral("无法解析服务器地址");
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return QStringLiteral("无法连接服务器");
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return QStringLiteral("连接被中断或重置");
    case ERROR_WINHTTP_SECURE_FAILURE:
        return QStringLiteral("HTTPS 证书或加密协议校验失败");
    case ERROR_WINHTTP_INVALID_URL:
        return QStringLiteral("无效的地址");
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return QStringLiteral("请求已取消");
    default:
        return QStringLiteral("网络错误（WinHTTP %1）").arg(code);
    }
}

/** 同步执行一次 GET，由工作线程调用。数据块交给 onData 处理：返回 false 表示写入失败，
 *  立即中止请求。传 contentLengthOut 可取到服务器声明的 Content-Length（未知为 -1）。 */
void winHttpFetch(const QUrl &url, const QString &userAgent, const QStringList &headers,
                  int timeoutMs, int *statusOut, qint64 *contentLengthOut,
                  const std::function<bool(const char *, DWORD)> &onData, QString *errorOut)
{
    *statusOut = 0;
    *contentLengthOut = -1;
    errorOut->clear();

    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        *errorOut = QStringLiteral("不支持的协议：%1").arg(url.scheme());
        return;
    }
    if (url.host().isEmpty()) {
        *errorOut = QStringLiteral("地址缺少主机名");
        return;
    }

    const bool secure = scheme == QLatin1String("https");
    const std::wstring host = url.host().toStdWString();
    const std::wstring agent = userAgent.toStdWString();

    QString target = url.path(QUrl::FullyEncoded);
    if (target.isEmpty())
        target = QStringLiteral("/");
    const QString query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty())
        target += QLatin1Char('?') + query;
    const std::wstring targetW = target.toStdWString();

    // Win8.1+ 支持自动代理（PAC/WPAD）；老系统（含 XP）回退到默认代理设置
    DWORD accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    accessType = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#endif
    HINTERNET session = WinHttpOpen(agent.c_str(), accessType, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        *errorOut = winHttpErrorText(GetLastError());
        return;
    }

    // 四个阶段各自计时：沿用同一个值会让总耗时最坏变成 4 倍（离线时尤其明显）。
    const int timeout = qBound(1000, timeoutMs, 60000);
    const int resolveTimeout = qMin(timeout, 2500);
    const int connectTimeout = qMin(timeout, 3000);
    WinHttpSetTimeouts(session, resolveTimeout, connectTimeout, timeout, timeout);

    HINTERNET connection = WinHttpConnect(session, host.c_str(),
                                          static_cast<INTERNET_PORT>(url.port(secure ? 443 : 80)), 0);
    if (!connection) {
        *errorOut = winHttpErrorText(GetLastError());
        WinHttpCloseHandle(session);
        return;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", targetW.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        *errorOut = winHttpErrorText(GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return;
    }

    // 老系统默认可能只开 TLS 1.0；GitHub 之类早已拒绝。设置失败（如 XP）就按系统默认走
    DWORD protocols = 0x00000800; // WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(request, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    QString headerBlock;
    for (const QString &line : headers) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            headerBlock += trimmed + QStringLiteral("\r\n");
    }
    const std::wstring headerW = headerBlock.toStdWString();

    BOOL ok = WinHttpSendRequest(request,
                                 headerW.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerW.c_str(),
                                 headerW.empty() ? 0 : static_cast<DWORD>(headerW.size()),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok)
        ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        *errorOut = winHttpErrorText(GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX)) {
        *statusOut = static_cast<int>(status);
    }
    DWORD contentLength = 0;
    DWORD contentLengthSize = sizeof(contentLength);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize,
                            WINHTTP_NO_HEADER_INDEX)) {
        *contentLengthOut = static_cast<qint64>(contentLength);
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            *errorOut = winHttpErrorText(GetLastError());
            break;
        }
        if (available == 0)
            break;

        QByteArray chunk;
        chunk.resize(static_cast<int>(available));
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            *errorOut = winHttpErrorText(GetLastError());
            break;
        }
        if (read == 0)
            break;
        chunk.resize(static_cast<int>(read));
        if (!onData(chunk.constData(), read)) {
            *errorOut = QStringLiteral("写入下载文件失败");
            break;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
}
#endif // Q_OS_WIN

} // namespace

/** 在工作线程里跑一次请求，结果用队列信号送回 HttpClient 所在线程。 */
class HttpWorker : public QThread
{
    Q_OBJECT

public:
    HttpWorker(quint64 id, QUrl url, QString userAgent, QStringList headers, int timeoutMs)
        : m_id(id)
        , m_url(std::move(url))
        , m_userAgent(std::move(userAgent))
        , m_headers(std::move(headers))
        , m_timeoutMs(timeoutMs)
    {
    }

signals:
    void done(quint64 id, int status, const QByteArray &body, const QString &error);

protected:
    void run() override
    {
        int status = 0;
        QByteArray body;
        QString error;

#ifdef Q_OS_WIN
        qint64 total = -1;
        winHttpFetch(m_url, m_userAgent, m_headers, m_timeoutMs, &status, &total,
                     [&body](const char *data, DWORD len) -> bool {
                         body.append(data, static_cast<int>(len));
                         // 更新检查与插件查询都是小 JSON；超了多半是打到了错误页面
                         return body.size() <= kMaxBodyBytes;
                     },
                     &error);
        if (error.isEmpty() && body.size() > kMaxBodyBytes)
            error = QStringLiteral("响应内容过大");
#else
        if (m_url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0
            && !QSslSocket::supportsSsl()) {
            // 没有 TLS 后端时 Qt 只会抛 "TLS initialization failed"，
            // 这里提前给出可读原因，别让用户对着日志猜
            error = QStringLiteral("当前系统缺少 TLS 运行库，无法发起 HTTPS 请求");
        } else {
            QNetworkAccessManager network;
            QNetworkRequest request(m_url);
            request.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);
            for (const QString &line : m_headers) {
                const int colon = line.indexOf(QLatin1Char(':'));
                if (colon <= 0)
                    continue;
                request.setRawHeader(line.left(colon).trimmed().toUtf8(),
                                     line.mid(colon + 1).trimmed().toUtf8());
            }

            QNetworkReply *reply = network.get(request);
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, [reply, &loop]() {
                reply->abort();
                loop.quit();
            });
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            timer.start(qBound(1000, m_timeoutMs, 60000));
            loop.exec();

            status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            body = reply->readAll();
            if (reply->error() != QNetworkReply::NoError)
                error = reply->errorString();
            delete reply;
        }
#endif

        emit done(m_id, status, body, error);
    }

private:
    quint64 m_id;
    QUrl m_url;
    QString m_userAgent;
    QStringList m_headers;
    int m_timeoutMs;
};

/** 在工作线程里把响应流式写到文件；进度与结果都通过队列信号送回调用线程。 */
class HttpDownloadWorker : public QThread
{
    Q_OBJECT

public:
    HttpDownloadWorker(quint64 id, QUrl url, QString userAgent, QString path, int timeoutMs)
        : m_id(id)
        , m_url(std::move(url))
        , m_userAgent(std::move(userAgent))
        , m_path(std::move(path))
        , m_timeoutMs(timeoutMs)
    {
    }

signals:
    void progress(quint64 id, qint64 received, qint64 total);
    void done(quint64 id, const QString &path, const QString &error);

protected:
    void run() override
    {
        QFile file(m_path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit done(m_id, m_path, QStringLiteral("无法创建下载文件：%1").arg(m_path));
            return;
        }

        QString error;
        qint64 received = 0;
#ifdef Q_OS_WIN
        int status = 0;
        qint64 total = -1;
        winHttpFetch(m_url, m_userAgent, QStringList(), m_timeoutMs, &status, &total,
                     [&](const char *data, DWORD len) -> bool {
                         if (file.write(data, static_cast<qint64>(len)) != static_cast<qint64>(len))
                             return false;
                         received += static_cast<qint64>(len);
                         emit progress(m_id, received, total);
                         return true;
                     },
                     &error);
        if (error.isEmpty() && status != 200)
            error = status > 0 ? QStringLiteral("服务器返回 HTTP %1").arg(status)
                               : QStringLiteral("服务器没有返回内容");
#else
        if (m_url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0
            && !QSslSocket::supportsSsl()) {
            error = QStringLiteral("当前系统缺少 TLS 运行库，无法发起 HTTPS 请求");
        } else {
            QNetworkAccessManager network;
            QNetworkRequest request(m_url);
            request.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);
            QNetworkReply *reply = network.get(request);
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, [reply, &loop]() {
                reply->abort();
                loop.quit();
            });
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QObject::connect(reply, &QNetworkReply::downloadProgress, this,
                             [this](qint64 got, qint64 all) { emit progress(m_id, got, all); });
            QObject::connect(reply, &QNetworkReply::readyRead, this, [&]() {
                const QByteArray chunk = reply->readAll();
                received += chunk.size();
                if (file.write(chunk) != chunk.size())
                    error = QStringLiteral("写入下载文件失败");
            });
            timer.start(qBound(1000, m_timeoutMs, 600000));
            loop.exec();
            if (error.isEmpty() && reply->error() != QNetworkReply::NoError)
                error = reply->errorString();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (error.isEmpty() && status != 0 && status != 200)
                error = QStringLiteral("服务器返回 HTTP %1").arg(status);
            delete reply;
        }
#endif
        file.close();
        if (!error.isEmpty()) {
            file.remove(); // 不留半成品，避免下次误当成完整安装包
            emit done(m_id, m_path, error);
            return;
        }
        emit done(m_id, m_path, QString());
    }

private:
    quint64 m_id;
    QUrl m_url;
    QString m_userAgent;
    QString m_path;
    int m_timeoutMs;
};

HttpClient::HttpClient(QObject *parent)
    : QObject(parent)
{
}

HttpClient::~HttpClient() = default;

quint64 HttpClient::get(const QUrl &url, const QString &userAgent,
                        const QStringList &headers, int timeoutMs)
{
    const quint64 id = m_nextId++;
    const int timeout = qBound(1000, timeoutMs, 60000);
    m_pending.insert(id);
    auto *worker = new HttpWorker(id, url, userAgent, headers, timeout);
    // 队列连接：done 在工作线程发射，finished 回到本对象所在线程
    connect(worker, &HttpWorker::done, this,
            [this](quint64 requestId, int status, const QByteArray &body, const QString &error) {
                if (!m_pending.remove(requestId))
                    return; // 总时限已上报过，迟到的工作线程结果丢弃
                emit finished(requestId, status, body, error);
            });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // WinHTTP 的解析/连接/发送/接收各阶段单独计时，叠加后可能远超调用方给的
    // timeoutMs；离线时插件只能一直等。这里兜底一个总时限，保证按时上报。
    QTimer::singleShot(timeout, this, [this, id]() {
        if (m_pending.remove(id))
            emit finished(id, 0, QByteArray(), QStringLiteral("网络请求超时"));
    });
    worker->start();
    return id;
}

quint64 HttpClient::downloadToFile(const QUrl &url, const QString &userAgent,
                                   const QString &filePath, int timeoutMs)
{
    const quint64 id = m_nextId++;
    const int timeout = qBound(1000, timeoutMs, 600000);
    QFileInfo info(filePath);
    QDir().mkpath(info.absolutePath());

    m_downloadPending.insert(id);
    auto *worker = new HttpDownloadWorker(id, url, userAgent, filePath, timeout);
    connect(worker, &HttpDownloadWorker::progress, this,
            [this](quint64 requestId, qint64 received, qint64 total) {
                if (m_downloadPending.contains(requestId))
                    emit downloadProgress(requestId, received, total);
            });
    connect(worker, &HttpDownloadWorker::done, this,
            [this](quint64 requestId, const QString &path, const QString &error) {
                if (!m_downloadPending.remove(requestId))
                    return;
                emit downloadFinished(requestId, path, error);
            });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    QTimer::singleShot(timeout, this, [this, id, filePath]() {
        if (m_downloadPending.remove(id)) {
            QFile::remove(filePath);
            emit downloadFinished(id, filePath, QStringLiteral("下载超时"));
        }
    });
    worker->start();
    return id;
}

#include "HttpClient.moc"
