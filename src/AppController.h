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
class SearchEngine;
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

public slots:
    void showLauncher();
    void startScan(bool notifyUser);

private slots:
    void onLaunchRequested(const ExecutableRecord &record, bool asAdmin);
    void onRevealRequested(const ExecutableRecord &record);
    void onScanProgress(const QString &root, int directories, int filesFound);
    void onScanFinished(int filesFound, int removed, bool canceled, const QString &error);
    void openSettings();
    void updateStatus();

private:
    void createTrayIcon();
    void setupSingleInstance();
    void setupHotkey();
    void setupScanSchedule();
    void setupConnections();
    void applySettings();
    void applyAutoStart();
    int indexCount() const;

    AppSettings m_settings;
    std::unique_ptr<Database> m_database;
    std::unique_ptr<SearchEngine> m_searchEngine;

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
    bool m_silentStart = false;
};
