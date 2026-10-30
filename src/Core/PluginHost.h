#pragma once

#include "Core/SearchEngine.h"
#include "listery_plugin_api.h"

#include <QHash>
#include <QMap>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

class HttpClient;

/**
 * 插件宿主：加载插件动态库、分发查询、把结果安全地送回 GUI 线程。
 *
 * 线程模型：插件可以在任何线程调用 addResult / finish（翻译插件在网络回调里调用，
 * 也就是主线程），宿主内部加锁累积，结束时通过队列连接切回 GUI 线程再发信号。
 * 插件不链接 Qt：网络请求由宿主的 httpGet 代劳，因此插件不需要 OpenSSL，
 * 也不会因为主程序用静态 Qt 而出现“两份 Qt 实例”的问题。
 */
struct PluginRow
{
    QString title;
    QString subtitle;
    QString badge;
    QString action;
};

struct PluginInfo
{
    QString id;
    QString name;
    QString version;
    QString description;
    QString filePath;
    QString defaultHotkey;
    /** 当前生效的热键（用户改过就用用户的，否则用插件默认值）。 */
    QString hotkey;
    bool enabled = true;
    bool loaded = false;
    QString error;
    bool hasConfigUi = false;
};

Q_DECLARE_METATYPE(PluginRow)
Q_DECLARE_METATYPE(QVector<PluginRow>)

class PluginHost : public QObject
{
    Q_OBJECT

public:
    explicit PluginHost(QObject *parent = nullptr);
    ~PluginHost() override;

    /** 扫描目录加载插件（Windows: *.dll，Linux: *.so，macOS: *.dylib）。返回成功加载数量。 */
    int loadFromDirectory(const QString &directory, QString *errorMessage = nullptr);

    QVector<PluginInfo> plugins() const;
    PluginInfo info(const QString &id) const;
    bool hasPlugin(const QString &id) const;
    /** 面板模式需要：第一个启用且热键等于 hotkey 的插件。 */
    QString pluginForHotkey(const QString &hotkey) const;

    void setEnabled(const QString &id, bool enabled);
    void setHotkey(const QString &id, const QString &hotkey);
    QStringList registeredHotkeys() const;

    /** 发起一次插件查询，返回序号（与 SearchService 的序号语义一致）。 */
    quint64 beginQuery(const QString &id, const QString &query, int maxResults = 12);
    void cancel(const QString &id);

    /** 打开插件自带的设置界面（无界面则返回 false）。 */
    bool configure(const QString &id, void *nativeParentWindow);

    // ---- 以下四个是 C ABI 回调的落点，普通调用方不要直接用 ----
    void appendRow(quint64 requestId, const QString &title, const QString &subtitle,
                   const QString &badge, const QString &action);
    void finishRequest(quint64 requestId, const QString &errorMessage);
    void writeLog(int level, const QString &message);
    int startHttpGet(const QString &url, const QString &headers,
                     ListeryHttpCallback callback, void *userData, int timeoutMs);

signals:
    void queryFinished(quint64 sequence, QString pluginId, QString query,
                       QVector<PluginRow> rows, QString errorMessage);

private slots:
    void deliver(quint64 sequence, QString pluginId, QString query,
                 QVector<PluginRow> rows, QString errorMessage);
    /** 非主线程发起的 httpGet 会排队到这里，由主线程真正发请求。 */
    void dispatchHttp(const QVariant &request);

private:
    struct LoadedPlugin;

    /** 宿主能力表必须是常驻对象：插件在 init() 里保存的是这个指针。 */
    ListeryHostV1 m_hostTable;
    std::vector<std::unique_ptr<LoadedPlugin>> m_plugins;
    QMap<quint64, QString> m_activeOwner;   // requestId -> plugin id（仅用于排查）

    /** 插件发起的 HTTP 请求：完成时按序号找回 C 回调。 */
    struct PendingHttp
    {
        ListeryHttpCallback callback = nullptr;
        void *userData = nullptr;
    };
    HttpClient *m_http = nullptr;
    QHash<quint64, PendingHttp> m_httpPending;
    QHash<quint64, QVector<PluginRow>> m_pending;
    QHash<quint64, QString> m_pendingQuery;
    quint64 m_nextSequence = 1;
    QMutex m_mutex;
};
