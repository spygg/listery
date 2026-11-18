#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class HttpClient;

/** GitHub Release 的附件（安装包等）。 */
struct ReleaseAsset
{
    QString name;               // 例如 "listery-windows-setup.exe"
    QString browserDownloadUrl; // GitHub 的直链（会 302 到 CDN）
    qint64 size = 0;            // 字节数，未知为 0
};

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
    QVector<ReleaseAsset> assets;
};

/**
 * 在线升级：向 GitHub Releases API 查询最新版本，比较版本，并可内置下载安装包。
 *
 * 设计要点：
 * - 网络请求交给 HttpClient（Windows 走 WinHTTP，无需 OpenSSL），
 *   结果在 GUI 线程回调，不阻塞界面；
 * - 失败时只发 checkFailed 信号，不弹窗——自动检查本就不该打扰用户；
 * - 版本比较用语义化版本号（1.2.0 > 1.10.0 这类易错点已处理）；
 * - Windows 安装包由本类直接流式下载到数据目录，无需再打开浏览器手动找。
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

    /** 在系统默认浏览器打开 URL（无内置安装包时的兜底）。 */
    static void openUrl(const QString &url);

    /** Windows 安装包在 Release assets 里的固定文件名。 */
    static QString windowsInstallerAssetName();

    /** 从发布信息里取 Windows 安装包的下载直链；没有则返回空串。 */
    static QString windowsInstallerUrl(const ReleaseInfo &release);

    /** 下载安装包到 targetPath。无可用直链时返回 false，不发任何信号。 */
    bool downloadInstaller(const QString &downloadUrl, const QString &targetPath);

signals:
    /** 发现新版本：release 为远端信息，current 为本机版本。 */
    void updateAvailable(const ReleaseInfo &release, const QString &current);
    /** 已是最新（或远端版本不高于本机）。 */
    void upToDate(const QString &current);
    /** 检查失败（网络错误 / 解析失败 / 限流）。error 为人类可读信息。 */
    void checkFailed(const QString &error);

    /** 安装包下载进度。 */
    void downloadProgress(qint64 received, qint64 total);
    /** 安装包下载结束：error 为空表示成功。 */
    void downloadFinished(const QString &filePath, const QString &error);

private slots:
    void onHttpFinished(quint64 requestId, int status, const QByteArray &body,
                        const QString &error);
    void onDownloadProgress(quint64 requestId, qint64 received, qint64 total);
    void onDownloadFinished(quint64 requestId, const QString &filePath, const QString &error);

private:
    HttpClient *m_http = nullptr;
};
