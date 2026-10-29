#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

/** GitHub Release 携带的版本信息。 */
struct ReleaseInfo
{
    QString version;     // 去掉 v 前缀后的版本号，例如 "1.2.0"
    QString tag;         // 原始 tag，例如 "v1.2.0"
    QString name;        // 发布标题
    QString htmlUrl;     // 发布页地址
    QString body;        // 更新说明（Markdown 原文）
    QString publishedAt; // ISO 发布时间
    bool prerelease = false;
};

/**
 * 在线升级检查：向 GitHub Releases API 查询最新版本，与本机版本比较。
 *
 * 设计要点：
 * - 全部走 QNetworkAccessManager，网络请求在调用方线程（GUI 线程）发起，
 *   但 reply 的 finished 信号回来时才触发 JSON 解析，不会阻塞界面；
 * - 失败时只发 checkFailed 信号，不弹窗——自动检查本就不该打扰用户；
 * - 版本比较用语义化版本号（1.2.0 > 1.10.0 这类易错点已处理）。
 */
class Updater : public QObject
{
    Q_OBJECT

public:
    explicit Updater(QObject *parent = nullptr);
    ~Updater() override;

    /** 当前运行版本（由 LISTERY_VERSION 宏注入）。 */
    static QString currentVersion();

    /** 去掉常见的 "v" / "V" 前缀，返回纯版本号。 */
    static QString stripTag(const QString &tag);

    /** 语义化版本比较：a > b 返回 1，相等返回 0，a < b 返回 -1。 */
    static int compareVersions(const QString &a, const QString &b);

    /** 异步检查最新发布；结果通过信号返回。apiUrl 形如
     *  https://api.github.com/repos/<owner>/<repo>/releases/latest */
    void checkForUpdates(const QString &apiUrl);

    /** 在系统默认浏览器打开 URL。 */
    static void openUrl(const QString &url);

signals:
    /** 发现新版本：release 为远端信息，current 为本机版本。 */
    void updateAvailable(const ReleaseInfo &release, const QString &current);
    /** 已是最新（或远端版本不高于本机）。 */
    void upToDate(const QString &current);
    /** 检查失败（网络错误 / 解析失败 / 限流）。error 为人类可读信息。 */
    void checkFailed(const QString &error);

private slots:
    void onFinished(QNetworkReply *reply);

private:
    QNetworkAccessManager *m_network = nullptr;
};
