#include "Core/PluginHost.h"

#include "listery_plugin_api.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMetaObject>
#include <QMutexLocker>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>

namespace {

void fillHostTable(ListeryHostV1 *table, void *context)
{
    table->apiVersion = LISTERY_PLUGIN_API_VERSION;
    table->context = context;
    table->addResult = nullptr;
    table->finish = nullptr;
    table->log = nullptr;
    table->httpGet = nullptr;
}

QString stringOrEmpty(const char *text)
{
    return text ? QString::fromUtf8(text) : QString();
}

} // namespace

struct PluginHost::LoadedPlugin
{
    std::unique_ptr<QLibrary> library;
    const ListeryPluginV1 *api = nullptr;
    PluginInfo info;
    bool initialized = false;
};

// ============================ 宿主回调（C ABI） ============================
namespace {

void LISTERY_PLUGIN_CALL hostAddResult(void *context,
                                       unsigned long long requestId,
                                       const char *title,
                                       const char *subtitle,
                                       const char *badge,
                                       const char *action)
{
    if (PluginHost *host = static_cast<PluginHost *>(context))
        host->appendRow(requestId, stringOrEmpty(title), stringOrEmpty(subtitle),
                        stringOrEmpty(badge), stringOrEmpty(action));
}

void LISTERY_PLUGIN_CALL hostFinish(void *context,
                                    unsigned long long requestId,
                                    const char *errorMessage)
{
    if (PluginHost *host = static_cast<PluginHost *>(context))
        host->finishRequest(requestId, stringOrEmpty(errorMessage));
}

void LISTERY_PLUGIN_CALL hostLog(void *context, int level, const char *message)
{
    if (PluginHost *host = static_cast<PluginHost *>(context))
        host->writeLog(level, stringOrEmpty(message));
}

int LISTERY_PLUGIN_CALL hostHttpGet(void *context,
                                    const char *url,
                                    const char *headers,
                                    ListeryHttpCallback callback,
                                    void *userData,
                                    int timeoutMs)
{
    PluginHost *host = static_cast<PluginHost *>(context);
    if (host == nullptr)
        return 1;

    const QString qUrl = stringOrEmpty(url);
    const QString qHeaders = stringOrEmpty(headers);

    if (QThread::currentThread() != host->thread()) {
        // 插件在别的线程发起：排队到主线程（返回值只能表示“已受理”）
        QVariantList request;
        request << qUrl << qHeaders
                << static_cast<qulonglong>(reinterpret_cast<quintptr>(callback))
                << static_cast<qulonglong>(reinterpret_cast<quintptr>(userData))
                << timeoutMs;
        QMetaObject::invokeMethod(host, "dispatchHttp", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant(request)));
        return 0;
    }

    return host->startHttpGet(qUrl, qHeaders, callback, userData, timeoutMs);
}

} // namespace

// ================================ PluginHost ================================
PluginHost::PluginHost(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<PluginRow>("PluginRow");
    qRegisterMetaType<QVector<PluginRow>>("QVector<PluginRow>");
    m_network = new QNetworkAccessManager(this);

    // 能力表常驻：插件 init() 里保存的就是 &m_hostTable
    fillHostTable(&m_hostTable, this);
    m_hostTable.addResult = hostAddResult;
    m_hostTable.finish = hostFinish;
    m_hostTable.log = hostLog;
    m_hostTable.httpGet = hostHttpGet;
}

PluginHost::~PluginHost()
{
    for (auto &plugin : m_plugins) {
        if (plugin->initialized && plugin->api->shutdown)
            plugin->api->shutdown();
    }
    // QLibrary 由 unique_ptr 释放（不主动 unload：插件可能仍有回调在飞）
    m_plugins.clear();
}

int PluginHost::loadFromDirectory(const QString &directory, QString *errorMessage)
{
    QDir dir(directory);
    if (!dir.exists()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("插件目录不存在：%1").arg(directory);
        return 0;
    }

    QStringList filters;
#if defined(Q_OS_WIN)
    filters << QStringLiteral("*.dll");
#elif defined(Q_OS_MAC)
    filters << QStringLiteral("*.dylib") << QStringLiteral("*.so");
#else
    filters << QStringLiteral("*.so");
#endif

    const QFileInfoList files = dir.entryInfoList(filters, QDir::Files | QDir::Readable);
    if (files.isEmpty()) {
        if (errorMessage)
            *errorMessage = QString();
        return 0;
    }

    int loaded = 0;
    for (const QFileInfo &file : files) {
        auto entry = std::make_unique<LoadedPlugin>();
        entry->library = std::make_unique<QLibrary>(file.absoluteFilePath());
        entry->info.filePath = file.absoluteFilePath();

        if (!entry->library->load()) {
            entry->info.error = QStringLiteral("无法加载：%1").arg(entry->library->errorString());
            m_plugins.push_back(std::move(entry));
            continue;
        }

        auto resolve = reinterpret_cast<ListeryPluginEntry>(
            entry->library->resolve("listery_plugin_v1"));
        if (resolve == nullptr) {
            entry->info.error = QStringLiteral("不是 Listery 插件（缺少 listery_plugin_v1 入口）");
            m_plugins.push_back(std::move(entry));
            continue;
        }

        const ListeryPluginV1 *api = resolve();
        if (api == nullptr || api->id == nullptr || api->name == nullptr) {
            entry->info.error = QStringLiteral("插件入口返回了空描述");
            m_plugins.push_back(std::move(entry));
            continue;
        }
        if (api->apiVersion != LISTERY_PLUGIN_API_VERSION) {
            entry->info.error = QStringLiteral("插件接口版本 %1 与本程序不兼容（需要 %2）")
                                    .arg(api->apiVersion)
                                    .arg(LISTERY_PLUGIN_API_VERSION);
            m_plugins.push_back(std::move(entry));
            continue;
        }
        if (hasPlugin(QString::fromUtf8(api->id))) {
            entry->info.error = QStringLiteral("插件标识 %1 已被占用").arg(QString::fromUtf8(api->id));
            m_plugins.push_back(std::move(entry));
            continue;
        }
        if (api->init == nullptr || api->beginQuery == nullptr) {
            entry->info.error = QStringLiteral("插件缺少必需的 init / beginQuery 实现");
            m_plugins.push_back(std::move(entry));
            continue;
        }

        entry->api = api;
        entry->info.id = QString::fromUtf8(api->id);
        entry->info.name = QString::fromUtf8(api->name);
        entry->info.version = QString::fromUtf8(api->version ? api->version : "");
        entry->info.description = QString::fromUtf8(api->description ? api->description : "");
        entry->info.defaultHotkey = QString::fromUtf8(api->defaultHotkey ? api->defaultHotkey : "");
        entry->info.hotkey = entry->info.defaultHotkey;
        entry->info.hasConfigUi = api->configure != nullptr;

        const int rc = api->init(&m_hostTable);
        if (rc != 0) {
            entry->info.error = QStringLiteral("插件初始化失败（错误码 %1）").arg(rc);
            m_plugins.push_back(std::move(entry));
            continue;
        }

        entry->initialized = true;
        entry->info.loaded = true;
        ++loaded;
        m_plugins.push_back(std::move(entry));
    }

    return loaded;
}

QVector<PluginInfo> PluginHost::plugins() const
{
    QVector<PluginInfo> result;
    result.reserve(static_cast<int>(m_plugins.size()));
    for (const auto &plugin : m_plugins)
        result.append(plugin->info);
    return result;
}

PluginInfo PluginHost::info(const QString &id) const
{
    for (const auto &plugin : m_plugins) {
        if (plugin->info.id == id)
            return plugin->info;
    }
    return PluginInfo();
}

bool PluginHost::hasPlugin(const QString &id) const
{
    for (const auto &plugin : m_plugins) {
        if (plugin->info.id == id)
            return true;
    }
    return false;
}

QString PluginHost::pluginForHotkey(const QString &hotkey) const
{
    if (hotkey.isEmpty())
        return QString();
    for (const auto &plugin : m_plugins) {
        if (plugin->info.enabled && plugin->info.hotkey.compare(hotkey, Qt::CaseInsensitive) == 0)
            return plugin->info.id;
    }
    return QString();
}

void PluginHost::setEnabled(const QString &id, bool enabled)
{
    for (auto &plugin : m_plugins) {
        if (plugin->info.id == id)
            plugin->info.enabled = enabled;
    }
}

void PluginHost::setHotkey(const QString &id, const QString &hotkey)
{
    for (auto &plugin : m_plugins) {
        if (plugin->info.id == id)
            plugin->info.hotkey = hotkey;
    }
}

QStringList PluginHost::registeredHotkeys() const
{
    QStringList keys;
    for (const auto &plugin : m_plugins) {
        if (plugin->info.enabled && !plugin->info.hotkey.isEmpty())
            keys.append(plugin->info.hotkey);
    }
    return keys;
}

quint64 PluginHost::beginQuery(const QString &id, const QString &query, int maxResults)
{
    LoadedPlugin *target = nullptr;
    for (auto &plugin : m_plugins) {
        if (plugin->info.id == id && plugin->info.loaded) {
            target = plugin.get();
            break;
        }
    }
    if (target == nullptr || target->api->beginQuery == nullptr)
        return 0;

    // 同一个插件的上一轮还没回来：让它尽快停止，界面只认最新序号
    for (auto it = m_activeOwner.constBegin(); it != m_activeOwner.constEnd(); ++it) {
        if (it.value() == id && target->api->cancelQuery)
            target->api->cancelQuery(it.key());
    }

    const quint64 sequence = m_nextSequence++;
    {
        QMutexLocker locker(&m_mutex);
        m_pendingQuery.insert(sequence, query);
        m_activeOwner.insert(sequence, id);
    }

    const QByteArray utf8 = query.toUtf8();
    const int rc = target->api->beginQuery(sequence, utf8.constData(), maxResults);
    if (rc != 0) {
        finishRequest(sequence, QStringLiteral("插件拒绝了这次请求（错误码 %1）").arg(rc));
        return 0;
    }
    return sequence;
}

void PluginHost::cancel(const QString &id)
{
    for (auto &plugin : m_plugins) {
        if (plugin->info.id != id || !plugin->api)
            continue;
        if (plugin->api->cancelQuery == nullptr)
            continue;
        for (auto it = m_activeOwner.constBegin(); it != m_activeOwner.constEnd(); ++it) {
            if (it.value() == id)
                plugin->api->cancelQuery(it.key());
        }
    }
}

bool PluginHost::configure(const QString &id, void *nativeParentWindow)
{
    for (auto &plugin : m_plugins) {
        if (plugin->info.id == id && plugin->api && plugin->api->configure)
            return plugin->api->configure(nativeParentWindow) == 0;
    }
    return false;
}

void PluginHost::appendRow(quint64 requestId, const QString &title, const QString &subtitle,
                           const QString &badge, const QString &action)
{
    if (title.isEmpty())
        return;
    QMutexLocker locker(&m_mutex);
    m_pending[requestId].append(PluginRow{title, subtitle, badge, action});
}

void PluginHost::finishRequest(quint64 requestId, const QString &errorMessage)
{
    QVector<PluginRow> rows;
    QString query;
    QString pluginId;
    {
        QMutexLocker locker(&m_mutex);
        rows = m_pending.take(requestId);
        query = m_pendingQuery.take(requestId);
        pluginId = m_activeOwner.take(requestId);
    }

    QMetaObject::invokeMethod(this, "deliver", Qt::QueuedConnection,
                              Q_ARG(quint64, requestId),
                              Q_ARG(QString, pluginId),
                              Q_ARG(QString, query),
                              Q_ARG(QVector<PluginRow>, rows),
                              Q_ARG(QString, errorMessage));
}

void PluginHost::deliver(quint64 sequence, QString pluginId, QString query,
                         QVector<PluginRow> rows, QString errorMessage)
{
    emit queryFinished(sequence, pluginId, query, rows, errorMessage);
}

void PluginHost::writeLog(int level, const QString &message)
{
    if (message.isEmpty())
        return;
    const QByteArray utf8 = message.toUtf8();
    if (level >= LISTERY_LOG_WARNING)
        qWarning("Listery[plugin]: %s", utf8.constData());
    else if (level == LISTERY_LOG_INFO)
        qInfo("Listery[plugin]: %s", utf8.constData());
    else
        qDebug("Listery[plugin]: %s", utf8.constData());
}

void PluginHost::dispatchHttp(const QVariant &request)
{
    const QVariantList parts = request.toList();
    if (parts.size() < 5)
        return;
    auto callback = reinterpret_cast<ListeryHttpCallback>(
        static_cast<quintptr>(parts.at(2).toULongLong()));
    auto userData = reinterpret_cast<void *>(
        static_cast<quintptr>(parts.at(3).toULongLong()));
    startHttpGet(parts.at(0).toString(), parts.at(1).toString(),
                 callback, userData, parts.at(4).toInt());
}

static const int kDefaultHttpTimeoutMs = 8000;

int PluginHost::startHttpGet(const QString &url,
                             const QString &headers,
                             ListeryHttpCallback callback,
                             void *userData,
                             int timeoutMs)
{
    if (m_network == nullptr) {
        if (callback)
            callback(userData, 0, nullptr, 0, "宿主没有可用的网络组件");
        return 1;
    }

    QNetworkRequest request(QUrl::fromUserInput(url));
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QStringList lines = headers.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
#else
    const QStringList lines = headers.split(QLatin1Char('\n'), QString::SkipEmptyParts);
#endif
    for (const QString &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        request.setRawHeader(line.left(colon).trimmed().toUtf8(),
                             line.mid(colon + 1).trimmed().toUtf8());
    }

    QNetworkReply *reply = m_network->get(request);
    if (reply == nullptr) {
        if (callback)
            callback(userData, 0, nullptr, 0, "无法创建网络请求");
        return 1;
    }

    const int timeout = timeoutMs > 0 ? timeoutMs : kDefaultHttpTimeoutMs;
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    auto *settled = new bool(false);

    connect(timer, &QTimer::timeout, this, [reply, settled, callback, userData]() {
        if (*settled)
            return;
        *settled = true;
        reply->abort();
        if (callback) {
            const QByteArray message = QStringLiteral("请求超时").toUtf8();
            callback(userData, 0, nullptr, 0, message.constData());
        }
        reply->deleteLater();
        delete settled;
    });
    timer->start(timeout);

    connect(reply, &QNetworkReply::finished,
            [reply, settled, callback, userData]() {
                if (*settled) {
                    reply->deleteLater();
                    return;
                }
                *settled = true;
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray body = reply->readAll();
                const QString error = reply->error() == QNetworkReply::NoError
                                          ? QString()
                                          : reply->errorString();
                if (callback) {
                    const QByteArray errorUtf8 = error.toUtf8();
                    callback(userData, status, body.constData(),
                             static_cast<unsigned int>(body.size()),
                             error.isEmpty() ? nullptr : errorUtf8.constData());
                }
                reply->deleteLater();
                delete settled;
            });

    return 0;
}
