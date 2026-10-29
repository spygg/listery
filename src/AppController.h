#pragma once

#include "Core/AppSettings.h"
#include "Core/Database.h"

#include <QObject>

class DoubleCtrlHook;
class LauncherWindow;
class QAction;
class QLocalServer;
class QSystemTrayIcon;
class QThread;
class QTimer;
class Scanner;
class SearchService;
class UsageRecorder;
struct ExecutableRecord;

namespace AppConstants {
constexpr const char *kSingleInstanceKey = "listery-spotlight";
}

/** 把索引库、扫描线程、全局热键、搜索面板和托盘串起来的应用控制器。 */
class AppController : public QObject
{
    Q_OBJECT

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    /** 开机自启（--autostart）时不主动弹出搜索框，只在托盘静默运行。 */
    void setSilentStart(bool silent);

    bool initialize();
    QString dataDirectory() const;
    bool isLauncherVisible() const;

    /**
     * 单个卷的刷新间隔（纯函数，便于自检）：
     * 有程序的卷按用户配置的间隔，程序很少的卷放宽，一个都没有的卷指数退避，最多 14 天。
     */
    static qint64 scanIntervalForVolume(const VolumeStat &stat, qint64 baseMs, bool smart);

public slots:
    void showLauncher();
    /** @param deep true 表示强制全量遍历（人工重建索引、或快速通道结果可疑时复核）。 */
    void startScan(bool notifyUser, bool deep = false, const QStringList &volumes = QStringList());

private slots:
    void runScheduledScan();

private slots:
    void onLaunchRequested(const ExecutableRecord &record, bool asAdmin);
    void onRevealRequested(const ExecutableRecord &record);
    void onScanProgress(const QString &root, int directories, int filesFound);
    void onScanFinished(int filesFound, int removed, bool canceled, const QString &error);
    void openSettings();
    void updateStatus();

private:
    void createTrayIcon();
    bool setupSingleInstance();
    void setupHotkey();
    void setupScanSchedule();
    void setupConnections();
    void applySettings();
    void applyAutoStart();
    int indexCount() const;

    // ----- 智能调度 -----
    /** 到期的卷：没扫过、到点了、或需要全量复核的。 */
    QStringList dueVolumes() const;
    /** 距离下一个到期卷还有多少毫秒；<=0 表示已经到期。 */
    qint64 nextScanDelayMs() const;
    /** 单个卷的刷新间隔：没有可执行文件的卷按指数退避，最多放宽到 14 天。 */
    qint64 intervalForVolume(const VolumeStat &stat, qint64 baseMs) const;
    /** 这些卷里是否有任何一个需要全量遍历。 */
    bool needsDeepScan(const QStringList &volumes) const;
    void scheduleNextScan(int immediateDelayMs = 2000);

    AppSettings m_settings;
    std::unique_ptr<Database> m_database;
    SearchService *m_searchService = nullptr;

    LauncherWindow *m_window = nullptr;
    Scanner *m_scanner = nullptr;
    DoubleCtrlHook *m_hook = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QLocalServer *m_server = nullptr;
    QTimer *m_scanTimer = nullptr;

    // 使用统计写入在独立后台线程进行，避免主线程 recordLaunch 与扫描线程的写锁互相阻塞
    UsageRecorder *m_usage = nullptr;
    QThread *m_usageThread = nullptr;

    QAction *m_actionShow = nullptr;
    QAction *m_actionRescan = nullptr;
    QAction *m_actionSettings = nullptr;
    QAction *m_actionQuit = nullptr;

    bool m_scanNotify = false;
    bool m_scanPending = false;
    bool m_pendingDeep = false;
    QStringList m_pendingVolumes;
    /** 面板正在使用时推迟扫描的次数上限，避免无限让位。 */
    int m_scanDeferred = 0;
    bool m_silentStart = false;
};
