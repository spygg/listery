#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

/**
 * 异步 HTTP(S) GET。
 *
 * Windows 上用 WinHTTP：Qt 5.15 的 Windows 包不带 OpenSSL 运行时，也没有 Schannel
 * TLS 插件，直接用 QNetworkAccessManager 发 HTTPS 必然 "TLS initialization failed"，
 * 随后还会在 Qt5Network 里崩溃。WinHTTP 走系统 TLS 栈，零额外 DLL。
 * 其它平台仍用 QNetworkAccessManager（系统 OpenSSL 可用）。
 *
 * finished 总是在 HttpClient 所在线程（通常是 GUI 线程）触发，回调里可以安全碰界面。
 */
class HttpClient : public QObject
{
    Q_OBJECT

public:
    explicit HttpClient(QObject *parent = nullptr);
    ~HttpClient() override;

    /** 发起 GET；headers 每项形如 "Name: value"。返回请求序号，仅用于区分本次回调。 */
    quint64 get(const QUrl &url, const QString &userAgent,
                const QStringList &headers = QStringList(), int timeoutMs = 10000);

signals:
    /** status 为 0 表示没拿到响应；此时 error 非空。body 为原始字节（UTF-8 / JSON）。 */
    void finished(quint64 requestId, int status, const QByteArray &body, const QString &error);

private:
    quint64 m_nextId = 1;
};
