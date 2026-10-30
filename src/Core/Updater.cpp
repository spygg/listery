#include "Core/Updater.h"

#include "Core/HttpClient.h"
#include "Core/QtCompat.h"

#ifndef LISTERY_VERSION
#define LISTERY_VERSION "0.0.0"
#endif

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace {
/** 把 "1.2.0" 拆成数字段，便于逐段比较。 */
QVector<int> parseVersionParts(const QString &version)
{
    QVector<int> parts;
    for (const QString &piece : version.split(QLatin1Char('.'), LISTERY_SKIP_EMPTY_PARTS)) {
        bool ok = false;
        const int value = piece.toInt(&ok);
        parts.append(ok ? value : 0);
    }
    return parts;
}
} // namespace

Updater::Updater(QObject *parent)
    : QObject(parent)
    , m_http(new HttpClient(this))
{
    connect(m_http, &HttpClient::finished, this, &Updater::onHttpFinished);
}

Updater::~Updater() = default;

QString Updater::currentVersion()
{
    return QStringLiteral(LISTERY_VERSION);
}

QString Updater::stripTag(const QString &tag)
{
    QString value = tag.trimmed();
    if (value.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)
        || value.startsWith(QLatin1Char('V')))
        value = value.mid(1);
    return value;
}

int Updater::compareVersions(const QString &a, const QString &b)
{
    const QVector<int> pa = parseVersionParts(a);
    const QVector<int> pb = parseVersionParts(b);
    const int n = qMax(pa.size(), pb.size());
    for (int i = 0; i < n; ++i) {
        const int va = i < pa.size() ? pa.at(i) : 0;
        const int vb = i < pb.size() ? pb.at(i) : 0;
        if (va != vb)
            return va > vb ? 1 : -1;
    }
    return 0;
}

void Updater::checkForUpdates(const QString &apiUrl)
{
    if (apiUrl.isEmpty() || !m_http)
        return;

    // GitHub API 要求带 UA，否则 403；Cache-Control 避免代理返回陈旧结果
    m_http->get(QUrl::fromUserInput(apiUrl),
                QStringLiteral("Listery/%1 (update-check)").arg(currentVersion()),
                QStringList() << QStringLiteral("Cache-Control: no-cache"), 10000);
}

void Updater::openUrl(const QString &url)
{
    if (!url.isEmpty())
        QDesktopServices::openUrl(QUrl::fromUserInput(url));
}

void Updater::onHttpFinished(quint64 requestId, int status, const QByteArray &body,
                             const QString &error)
{
    Q_UNUSED(requestId)

    if (!error.isEmpty()) {
        emit checkFailed(error);
        return;
    }
    if (status != 200) {
        emit checkFailed(status > 0
                             ? QStringLiteral("服务器返回 HTTP %1").arg(status)
                             : QStringLiteral("服务器没有返回内容"));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        emit checkFailed(parseError.errorString());
        return;
    }

    const QJsonObject root = doc.object();
    const QString tag = root.value(QStringLiteral("tag_name")).toString();
    if (tag.isEmpty()) {
        emit checkFailed(QStringLiteral("响应中缺少 tag_name（可能不是 Releases API）"));
        return;
    }

    ReleaseInfo release;
    release.tag = tag;
    release.version = stripTag(tag);
    release.name = root.value(QStringLiteral("name")).toString();
    release.htmlUrl = root.value(QStringLiteral("html_url")).toString();
    release.body = root.value(QStringLiteral("body")).toString();
    release.publishedAt = root.value(QStringLiteral("published_at")).toString();
    release.prerelease = root.value(QStringLiteral("prerelease")).toBool();

    const QString current = currentVersion();
    if (compareVersions(release.version, current) > 0)
        emit updateAvailable(release, current);
    else
        emit upToDate(current);
}
