#include "AppController.h"

#include "Core/LaunchService.h"
#include "Core/PluginHost.h"
#include "Core/Scanner.h"
#include "Core/SearchService.h"
#include "Core/Updater.h"
#include "Platform/Autostart.h"
#include "Platform/Branding.h"
#include "Platform/HotkeyHook.h"
#include "Ui/LauncherWindow.h"
#include "Ui/SettingsDialog.h"

#include "Core/UsageRecorder.h"
#include "Core/QtCompat.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QMenu>
#include <QMetaObject>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace {
/**
 * 智能调度的档位基线（毫秒）。
 *  - 有可执行文件的卷：用户配置的刷新间隔（默认 12 小时）；
 *  - 只有零星几个：2 倍；
 *  - 一个都没有：4 倍起步，每空扫一轮再翻倍，最多放宽到 14 天。
 */
const qint64 kMaxScanIntervalMs = 14LL * 24 * 60 * 60 * 1000;
/** 调度器最长休眠时间：挂载的卷会变，至少每 3 小时重新评估一次。 */
const qint64 kMaxPollIntervalMs = 3LL * 60 * 60 * 1000;

/** 在线升级：GitHub Releases 最新版查询接口（仓库 spygg/listery）。 */
const char *kGitHubReleasesApi = "https://api.github.com/repos/spygg/listery/releases/latest";
/** 自动检查至少间隔一天，避免反复打扰 GitHub API 限流（匿名 60 次/小时/IP）。 */
const qint64 kUpdateCheckIntervalMs = 24LL * 60 * 60 * 1000;
/** 启动后延迟几秒再查，先把索引与界面跑顺。 */
const int kUpdateCheckDelayMs = 8000;
} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent)
{
}

void AppController::setSilentStart(bool silent)
{
    m_silentStart = silent;
}

AppController::~AppController()
{
    if (m_searchService)
        m_searchService->stop();
    if (m_usageThread) {
        // finished 信号已在后台线程里触发 UsageRecorder::shutdown（关闭其连接），随后 deleteLater
        m_usageThread->quit();
        m_usageThread->wait(3000);
        m_usageThread = nullptr;
        m_usage = nullptr;
    }
    if (m_scanner) {
        m_scanner->requestStop();
        m_scanner->wait(8000);
    }
    if (m_hook) {
        m_hook->requestStop();
        m_hook->wait(2000);
    }
}

bool AppController::initialize()
{
    // 先抢单实例锁：第二个实例不该再去碰索引库或启动检索线程
    if (!setupSingleInstance())
        return false;

    QDir().mkpath(m_settings.dataDir());

    m_database.reset(new Database(m_settings.databasePath()));
    QString error;
    if (!m_database->open(&error)) {
        QMessageBox::critical(nullptr,
                              QStringLiteral("Listery"),
                              QStringLiteral("无法打开索引库：\n%1\n\n路径：%2")
                                  .arg(error, m_settings.databasePath()));
        return false;
    }
    m_database->setMeta(QStringLiteral("app_version"), Updater::currentVersion());

    // 检索在独立线程执行；首次升级时 FTS 索引的建立也发生在这里
    m_searchService = new SearchService(m_settings.databasePath(), this);
    QString searchError;
    if (!m_searchService->start(&searchError))
        qWarning("Listery: 检索线程启动失败，搜索功能不可用：%s", qPrintable(searchError));

    // 使用统计写入器：独立后台线程 + 独立连接，不占用 GUI 线程
    m_usageThread = new QThread(this);
    m_usage = new UsageRecorder(m_settings.databasePath());
    m_usage->moveToThread(m_usageThread);
    connect(m_usageThread, &QThread::finished, m_usage, &UsageRecorder::shutdown);
    connect(m_usageThread, &QThread::finished, m_usage, &QObject::deleteLater);
    m_usageThread->start();

    m_window = new LauncherWindow(m_searchService, m_settings.maxResults());
    m_window->setHideOnFocusLost(m_settings.hideWhenFocusLost());
    m_window->setStatusText(QStringLiteral("索引尚未建立"));

    setupPlugins();
    createTrayIcon();
    setupUpdater();
    setupHotkey();
    refreshPluginHotkeys(); // 钩子起来后才能真正注册组合键
    setupScanSchedule();
    setupConnections();

    const int count = indexCount();
    const qint64 lastScan = m_database->meta(QStringLiteral("last_scan_time"),
                                             QStringLiteral("0")).toLongLong();
    const bool interrupted = !m_database->meta(QStringLiteral("scan_in_progress")).isEmpty();
    const bool indexComplete = count > 0 && lastScan > 0 && !interrupted;

    if (count == 0) {
        m_window->setStatusText(QStringLiteral("索引尚未建立 · 首次扫描进行中"));
        if (!m_silentStart)
            QTimer::singleShot(600, this, &AppController::showLauncher);
    }
    updateStatus();
    applyAutoStart();

    // 索引缺失、上次扫描被中断时立刻补扫；否则交给调度器按“每个卷各自的到期时间”决定
    if (!indexComplete) {
        QTimer::singleShot(300, this, [this]() {
            startScan(false);
        });
        scheduleNextScan(10000);
    } else if (m_settings.autoScanOnStart()) {
        scheduleNextScan(1500);
    } else {
        // 用户关掉了开机扫描：等到下一个卷自然到期再动（最多 3 小时后重新评估）
        scheduleNextScan(static_cast<int>(kMaxPollIntervalMs));
    }

    if (m_tray && !QSystemTrayIcon::isSystemTrayAvailable())
        m_tray->showMessage(QStringLiteral("Listery"),
                            QStringLiteral("系统托盘不可用，请用热键或托盘外方式启动"),
                            QSystemTrayIcon::Warning, 4000);

    return true;
}

QString AppController::dataDirectory() const
{
    return m_settings.dataDir();
}

bool AppController::isLauncherVisible() const
{
    return m_window && m_window->isVisible();
}

void AppController::setupConnections()
{
    connect(m_window, &LauncherWindow::launchRequested, this, &AppController::onLaunchRequested);
    connect(m_window, &LauncherWindow::revealRequested, this, &AppController::onRevealRequested);
    connect(m_window, &LauncherWindow::settingsRequested, this, &AppController::openSettings);
    connect(m_window, &LauncherWindow::pluginActionRequested, this, &AppController::onPluginAction);
    connect(m_window, &LauncherWindow::pluginSessionClosed, this, &AppController::onPluginSessionClosed);
}

void AppController::setupPlugins()
{
    m_plugins = new PluginHost(this);
    m_window->setPluginHost(m_plugins);

    QDir().mkpath(m_settings.pluginDir());
    QString error;
    const int loaded = m_plugins->loadFromDirectory(m_settings.pluginDir(), &error);
    if (loaded > 0) {
        qInfo("Listery: 已加载 %d 个插件", loaded);
    } else if (!error.isEmpty()) {
        qWarning("Listery: %s", qPrintable(error));
    }

    // 应用用户设置：启用状态与自定义热键
    for (const PluginInfo &info : m_plugins->plugins()) {
        m_plugins->setEnabled(info.id, m_settings.pluginEnabled(info.id));
        const QString custom = m_settings.pluginHotkey(info.id);
        m_plugins->setHotkey(info.id, custom.isEmpty() ? info.defaultHotkey : custom);
    }
    refreshPluginHotkeys();
}

void AppController::refreshPluginHotkeys()
{
    if (!m_plugins || !m_hook)
        return;

    QVector<HotkeySpec> specs;
    for (const PluginInfo &info : m_plugins->plugins()) {
        if (!info.enabled || info.hotkey.isEmpty() || !info.loaded)
            continue;
        HotkeySpec spec = parseHotkey(info.hotkey);
        if (!spec.valid()) {
            qWarning("Listery: 插件 %s 的热键“%s”无法识别，已忽略",
                     qPrintable(info.id), qPrintable(info.hotkey));
            continue;
        }
        spec.text = info.hotkey;
        specs.append(spec);
    }
    m_hook->setHotkeys(specs);
}

void AppController::onHotkeyTriggered(const QString &hotkey)
{
    if (!m_plugins)
        return;
    const QString pluginId = m_plugins->pluginForHotkey(hotkey);
    if (pluginId.isEmpty())
        return;

    const PluginInfo info = m_plugins->info(pluginId);
    if (!info.loaded)
        return;
    m_window->beginPluginSession(pluginId, info.name,
                                 QStringLiteral("输入要%1的内容，Enter 复制结果").arg(info.name));
}

void AppController::onPluginAction(const QString &action)
{
    if (action.isEmpty())
        return;

    if (action.startsWith(QStringLiteral("copy:"))) {
        const QString text = action.mid(5);
        QGuiApplication::clipboard()->setText(text);
        if (m_tray)
            m_tray->showMessage(QStringLiteral("Listery"),
                                QStringLiteral("已复制：%1").arg(text),
                                QSystemTrayIcon::Information, 2500);
        return;
    }
    if (action.startsWith(QStringLiteral("url:"))) {
        QDesktopServices::openUrl(QUrl(action.mid(4)));
        return;
    }
    if (action.startsWith(QStringLiteral("run:"))) {
        const QString path = action.mid(4);
        // 交给系统默认处理方式，避免在这里猜测目标类型
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)) && m_tray)
            m_tray->showMessage(QStringLiteral("Listery"),
                                QStringLiteral("无法打开：%1").arg(path),
                                QSystemTrayIcon::Warning, 3000);
        return;
    }
    if (action.startsWith(QStringLiteral("open:"))) {
        const QString path = action.mid(5);
        QString error;
        if (!LaunchService::revealInFolder(path, &error) && m_tray)
            m_tray->showMessage(QStringLiteral("Listery"), error,
                                QSystemTrayIcon::Warning, 3000);
        return;
    }
    qWarning("Listery: 无法识别的插件动作：%s", qPrintable(action));
}

void AppController::onPluginSessionClosed()
{
    updateStatus();
}

void AppController::createTrayIcon()
{
    m_tray = new QSystemTrayIcon(Branding::appIcon(), this);
    m_tray->setToolTip(QStringLiteral("Listery · 双击 Ctrl 唤起搜索"));

    auto *menu = new QMenu();
    m_actionShow = menu->addAction(QStringLiteral("打开搜索框"));
    m_actionRescan = menu->addAction(QStringLiteral("重新扫描全盘"));
    m_actionSettings = menu->addAction(QStringLiteral("设置…"));
    if (m_plugins) {
        m_pluginMenu = menu->addMenu(QStringLiteral("插件"));
        for (const PluginInfo &info : m_plugins->plugins()) {
            if (!info.loaded)
                continue;
            const QString label = info.hotkey.isEmpty()
                                      ? info.name
                                      : QStringLiteral("%1（%2）").arg(info.name, info.hotkey);
            QAction *action = m_pluginMenu->addAction(label);
            const QString pluginId = info.id;
            connect(action, &QAction::triggered, this, [this, pluginId]() {
                const PluginInfo target = m_plugins->info(pluginId);
                m_window->beginPluginSession(pluginId, target.name,
                                             QStringLiteral("输入内容，交给 %1").arg(target.name));
            });
        }
        if (m_pluginMenu->isEmpty())
            m_pluginMenu->setEnabled(false);
    }
    menu->addSeparator();
    m_actionQuit = menu->addAction(QStringLiteral("退出"));

    connect(m_actionShow, &QAction::triggered, this, &AppController::showLauncher);
    connect(m_actionRescan, &QAction::triggered, this,
            [this]() { startScan(true, true); }); // 人工重建：强制全量遍历
    connect(m_actionSettings, &QAction::triggered, this, &AppController::openSettings);
    connect(m_actionQuit, &QAction::triggered, qApp, &QApplication::quit);

    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    showLauncher();
            });
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, &AppController::openPendingUpdate);
    m_tray->show();
}

void AppController::setupUpdater()
{
    m_updater = new Updater(this);
    connect(m_updater, &Updater::updateAvailable,
            this, &AppController::onUpdateAvailable);
    connect(m_updater, &Updater::upToDate,
            this, &AppController::onUpdateUpToDate);
    connect(m_updater, &Updater::checkFailed,
            this, &AppController::onUpdateCheckFailed);

    // 托盘菜单里提供手动检查入口
    if (m_tray && m_tray->contextMenu())
        m_actionCheckUpdate = m_tray->contextMenu()->addAction(QStringLiteral("检查更新…"));
    if (m_actionCheckUpdate) {
        connect(m_actionCheckUpdate, &QAction::triggered, this,
                &AppController::checkForUpdatesNow);
    }

    // 启动时按每天至多一次的频率自动检查；用户关掉开关或距上次不足一天则跳过
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool due = now - m_settings.lastUpdateCheck() > kUpdateCheckIntervalMs;
    if (m_settings.autoCheckUpdates() && due)
        QTimer::singleShot(kUpdateCheckDelayMs, this, &AppController::checkForUpdatesNow);
}

void AppController::checkForUpdatesNow()
{
    if (!m_updater)
        return;
    m_updater->checkForUpdates(kGitHubReleasesApi);
    m_settings.setLastUpdateCheck(QDateTime::currentMSecsSinceEpoch());
}

void AppController::onUpdateAvailable(const ReleaseInfo &release, const QString &current)
{
    m_pendingUpdateUrl = release.htmlUrl;
    if (m_tray) {
        m_tray->showMessage(QStringLiteral("Listery 有更新"),
                            QStringLiteral("发现新版本 %1（当前 %2），点击前往下载")
                                .arg(release.version, current),
                            QSystemTrayIcon::Information, 10000);
    }
    qInfo("Listery: 新版本可用 %s（当前 %s），发布页 %s",
          qPrintable(release.version), qPrintable(current), qPrintable(release.htmlUrl));
}

void AppController::onUpdateUpToDate(const QString &current)
{
    qInfo("Listery: 已是最新版本 %s", qPrintable(current));
}

void AppController::onUpdateCheckFailed(const QString &error)
{
    qWarning("Listery: 更新检查失败：%s", qPrintable(error));
}

void AppController::openPendingUpdate()
{
    if (!m_pendingUpdateUrl.isEmpty()) {
        Updater::openUrl(m_pendingUpdateUrl);
        m_pendingUpdateUrl.clear();
    }
}

bool AppController::setupSingleInstance()
{
    m_server = new QLocalServer(this);
    connect(m_server, &QLocalServer::newConnection, this, [this]() {
        while (m_server->hasPendingConnections()) {
            QLocalSocket *socket = m_server->nextPendingConnection();
            connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
                const QByteArray payload = socket->readAll();
                if (payload.contains("show"))
                    showLauncher();
                socket->disconnectFromServer();
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
        }
    });

    QLocalServer::removeServer(QString::fromLatin1(AppConstants::kSingleInstanceKey));
    if (!m_server->listen(QString::fromLatin1(AppConstants::kSingleInstanceKey))) {
        // 拿不到单实例锁说明已有实例在运行：只能退出，否则会出现两个钩子和两个扫描线程写同一索引库
        qWarning("Listery: 单实例监听失败，退出：%s", qPrintable(m_server->errorString()));
        return false;
    }
    return true;
}

void AppController::setupHotkey()
{
    if (!GlobalHotkeys::isSupported()) {
        QString hint = QStringLiteral("当前平台未实现全局双击 Ctrl，请用托盘菜单打开搜索框");
#if defined(Q_OS_LINUX)
        const QString session = qEnvironmentVariable("XDG_SESSION_TYPE");
        if (session.compare(QStringLiteral("wayland"), Qt::CaseInsensitive) == 0
            || QGuiApplication::platformName().contains(QStringLiteral("wayland"))) {
            hint = QStringLiteral("Wayland 会话下无法监听全局按键，请改用 X11 会话，或从托盘菜单打开搜索框");
        }
#endif
        if (m_tray)
            m_tray->showMessage(QStringLiteral("Listery"), hint,
                                QSystemTrayIcon::Information, 5000);
        return;
    }

    m_hook = new GlobalHotkeys(this);
    m_hook->setIntervalMs(m_settings.hotkeyIntervalMs());
    connect(m_hook, &GlobalHotkeys::activated, this, &AppController::showLauncher);
    connect(m_hook, &GlobalHotkeys::hotkeyTriggered, this, &AppController::onHotkeyTriggered);
    connect(m_hook, &GlobalHotkeys::failed, this, [this](const QString &reason) {
        qWarning("Listery: %s", qPrintable(reason));
        if (m_tray)
            m_tray->showMessage(QStringLiteral("Listery"),
                                QStringLiteral("双击 Ctrl 不可用：%1\n可先用托盘菜单打开搜索框")
                                    .arg(reason),
                                QSystemTrayIcon::Warning, 6000);
    });
    m_hook->start();
}

void AppController::setupScanSchedule()
{
    m_scanTimer = new QTimer(this);
    m_scanTimer->setSingleShot(true);
    connect(m_scanTimer, &QTimer::timeout, this, &AppController::runScheduledScan);
    scheduleNextScan(1500);
}

void AppController::runScheduledScan()
{
    if (!m_database)
        return;

    const QStringList due = dueVolumes();
    if (due.isEmpty()) {
        scheduleNextScan();
        return;
    }
    // 一次只扫到期的卷：没装过软件的分区不会被反复遍历
    startScan(false, needsDeepScan(due), due);
}

void AppController::scheduleNextScan(int immediateDelayMs)
{
    if (!m_scanTimer)
        return;

    m_scanTimer->stop();
    if (m_settings.rescanIntervalMinutes() <= 0)
        return; // 用户关掉了定时刷新

    const qint64 wait = nextScanDelayMs();
    // 已经到期 → 稍等一会就扫；还没到期 → 睡到下一个到期点（但不超过 3 小时，便于感知新挂载的卷）
    const qint64 delay = wait > 0 ? qMin(wait, kMaxPollIntervalMs) : qMax<qint64>(500, immediateDelayMs);
    m_scanTimer->start(static_cast<int>(qMin<qint64>(delay, kMaxPollIntervalMs)));
}

QStringList AppController::dueVolumes() const
{
    if (!m_database)
        return QStringList();

    const QStringList roots = Scanner::availableRoots(m_settings.includeRemovableDrives());
    if (roots.isEmpty())
        return QStringList();

    const qint64 baseMs = qMax(30, m_settings.rescanIntervalMinutes()) * 60LL * 1000LL;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QStringList due;
    for (const QString &root : roots) {
        const VolumeStat stat = m_database->volumeStat(root);
        if (stat.lastScan <= 0 || stat.deepPending) {
            due.append(root); // 从没扫过，或上一轮快速通道的结果需要用全量遍历复核
            continue;
        }
        if (now - stat.lastScan >= intervalForVolume(stat, baseMs))
            due.append(root);
    }

    // 关掉智能调度时回到老行为：一次把所有卷都扫一遍
    if (!m_settings.smartSchedule() && !due.isEmpty())
        return roots;

    return due;
}

qint64 AppController::nextScanDelayMs() const
{
    if (!m_database)
        return kMaxPollIntervalMs;

    const QStringList roots = Scanner::availableRoots(m_settings.includeRemovableDrives());
    if (roots.isEmpty())
        return kMaxPollIntervalMs;

    const qint64 baseMs = qMax(30, m_settings.rescanIntervalMinutes()) * 60LL * 1000LL;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    qint64 best = kMaxPollIntervalMs;
    for (const QString &root : roots) {
        const VolumeStat stat = m_database->volumeStat(root);
        if (stat.lastScan <= 0 || stat.deepPending)
            return 0;

        const qint64 wait = stat.lastScan + intervalForVolume(stat, baseMs) - now;
        best = qMin(best, qMax<qint64>(0, wait));
    }
    return best;
}

qint64 AppController::intervalForVolume(const VolumeStat &stat, qint64 baseMs) const
{
    return scanIntervalForVolume(stat, baseMs, m_settings.smartSchedule());
}

qint64 AppController::scanIntervalForVolume(const VolumeStat &stat, qint64 baseMs, bool smart)
{
    if (!smart)
        return baseMs;

    if (stat.exeCount <= 0) {
        // 一个可执行文件都没有的分区（纯数据盘、备份盘）：4 倍起步，每空一轮翻倍
        qint64 multiplier = 4;
        for (int i = 1; i < qMin(stat.emptyRounds, 5); ++i)
            multiplier *= 2;
        return qMin(baseMs * multiplier, kMaxScanIntervalMs);
    }
    if (stat.exeCount <= 5)
        return qMin(baseMs * 2, kMaxScanIntervalMs); // 只有零星几个，不必勤快

    return baseMs;
}

bool AppController::needsDeepScan(const QStringList &volumes) const
{
    if (!m_database)
        return true;
    if (!m_settings.useLocateIndex())
        return true; // 用户要求每次都真遍历

    for (const QString &root : volumes) {
        const VolumeStat stat = m_database->volumeStat(root);
        if (stat.lastScan <= 0 || stat.deepPending)
            return true;
    }
    return false;
}

void AppController::showLauncher()
{
    if (!m_window)
        return;
    qInfo("Listery: 唤起搜索面板");
    updateStatus();
    m_window->popup();
}

int AppController::indexCount() const
{
    return m_database ? m_database->totalCount() : 0;
}

void AppController::updateStatus()
{
    if (!m_window || !m_database)
        return;

    const int count = indexCount();
    if (count <= 0) {
        m_window->setStatusText(QStringLiteral("索引尚未建立"));
        if (m_tray)
            m_tray->setToolTip(QStringLiteral("Listery · 正在建立索引"));
        return;
    }

    const qint64 lastScan = m_database->meta(QStringLiteral("last_scan_time"), QStringLiteral("0")).toLongLong();
    const QString suffix = lastScan > 0
        ? QStringLiteral(" · 更新于 %1").arg(QDateTime::fromMSecsSinceEpoch(lastScan).toString(QStringLiteral("MM-dd HH:mm")))
        : QString();

    m_window->setStatusText(QStringLiteral("已索引 %1 个可执行文件%2")
                                .arg(QLocale().toString(count), suffix));
    if (m_tray)
        m_tray->setToolTip(QStringLiteral("Listery · 已索引 %1 个可执行文件").arg(QLocale().toString(count)));
}

void AppController::startScan(bool notifyUser, bool deep, const QStringList &volumes)
{
    if (!m_database)
        return;

    if (m_scanner) {
        if (m_scanner->isRunning()) {
            m_scanPending = true;
            m_pendingDeep = deep;
            m_pendingVolumes = volumes;
            if (notifyUser && m_tray)
                m_tray->showMessage(QStringLiteral("Listery"),
                                    QStringLiteral("已有扫描在进行，完成后会自动再扫一遍"),
                                    QSystemTrayIcon::Information, 3000);
            return;
        }
        // run() 已结束、finished 尚未派发：放弃指针即可，对象由 finished 连接负责 deleteLater
        m_scanner = nullptr;
    }

    // 面板正在被使用时不要抢磁盘 IO，等用户搜完再说（最多让三次）
    if (!notifyUser && isLauncherVisible() && m_scanDeferred < 3) {
        ++m_scanDeferred;
        QTimer::singleShot(4000, this, [this, deep, volumes]() {
            startScan(false, deep, volumes);
        });
        return;
    }
    m_scanDeferred = 0;

    const qint64 generation = QDateTime::currentMSecsSinceEpoch();
    m_scanNotify = notifyUser;

    Scanner *scanner = new Scanner(m_settings.databasePath(),
                                   m_settings.executableExtensions(),
                                   m_settings.skipDirectoryPatterns(),
                                   m_settings.includeRemovableDrives(),
                                   generation,
                                   volumes,
                                   deep,
                                   this);
    m_scanner = scanner;
    connect(scanner, &Scanner::progressChanged, this, &AppController::onScanProgress);
    connect(scanner, &Scanner::scanFinished, this, &AppController::onScanFinished);
    // QThread 必须等 run() 真正返回后再删；同时在这里清掉指针，避免悬垂
    connect(scanner, &QThread::finished, this, [this, scanner]() {
        if (m_scanner == scanner)
            m_scanner = nullptr;
        scanner->deleteLater();
    });
    scanner->start();

    m_window->setStatusText(QStringLiteral("正在准备扫描…"));
    if (notifyUser && m_tray)
        m_tray->showMessage(QStringLiteral("Listery"),
                            QStringLiteral("开始重建索引，可继续使用已有结果"),
                            QSystemTrayIcon::Information, 3000);
}

void AppController::onScanProgress(const QString &root, int directories, int filesFound)
{
    Q_UNUSED(directories)
    if (!m_window)
        return;
    m_window->setStatusText(QStringLiteral("正在扫描 %1 · 已找到 %2 个可执行文件")
                                .arg(QDir::toNativeSeparators(root),
                                     QLocale().toString(filesFound)));
}

void AppController::onScanFinished(int filesFound, int removed, bool canceled, const QString &error)
{
    Scanner *scanner = qobject_cast<Scanner *>(sender());
    // 上一次扫描迟到的完成信号不能影响当前这次扫描
    if (!scanner || (m_scanner && scanner != m_scanner))
        return;

    if (!error.isEmpty()) {
        if (m_window)
            m_window->setStatusText(QStringLiteral("索引失败：%1").arg(error));
        if (m_tray)
            m_tray->showMessage(QStringLiteral("Listery"),
                                QStringLiteral("扫描失败：%1").arg(error),
                                QSystemTrayIcon::Warning, 6000);
    } else if (!canceled) {
        if (m_window) {
            m_window->refreshResults();
        }
        updateStatus();
        if (m_scanNotify && m_tray) {
            m_tray->showMessage(QStringLiteral("Listery"),
                                QStringLiteral("索引完成：%1 个可执行文件，清理 %2 条失效记录")
                                    .arg(QLocale().toString(filesFound), QLocale().toString(removed)),
                                QSystemTrayIcon::Information, 4000);
        }
    } else {
        updateStatus();
    }

    if (m_scanPending) {
        m_scanPending = false;
        const bool deep = m_pendingDeep;
        const QStringList volumes = m_pendingVolumes;
        m_pendingVolumes.clear();
        QTimer::singleShot(200, this, [this, deep, volumes]() { startScan(true, deep, volumes); });
        return;
    }
    scheduleNextScan();
}

void AppController::onLaunchRequested(const ExecutableRecord &record, bool asAdmin)
{
    QString error;
    if (!LaunchService::launch(record, asAdmin, &error)) {
        // 失败时只用托盘通知，不弹模态对话框骚扰用户
        if (m_tray)
            m_tray->showMessage(QStringLiteral("Listery"),
                                QStringLiteral("启动失败：\n%1\n%2").arg(record.path, error),
                                QSystemTrayIcon::Warning, 4000);
        else
            qWarning("Listery: 启动失败：%s | %s", qPrintable(record.path), qPrintable(error));
        return;
    }

    // 使用统计写入放到后台线程，绝不阻塞 GUI（修复 Enter 执行时界面卡住）
    if (m_usage)
        QMetaObject::invokeMethod(m_usage, "record", Qt::QueuedConnection,
                                  Q_ARG(QString, record.path));
    m_window->dismiss();
}

void AppController::onRevealRequested(const ExecutableRecord &record)
{
    QString error;
    if (!LaunchService::revealInFolder(record.path, &error)) {
        if (m_tray)
            m_tray->showMessage(QStringLiteral("Listery"),
                                QStringLiteral("无法定位：\n%1\n%2").arg(record.path, error),
                                QSystemTrayIcon::Warning, 4000);
        else
            qWarning("Listery: 无法定位：%s | %s", qPrintable(record.path), qPrintable(error));
        return;
    }
    m_window->dismiss();
}

void AppController::openSettings()
{
    if (m_window)
        m_window->dismiss();

    const QStringList previousExtensions = m_settings.executableExtensions();

    SettingsDialog dialog(&m_settings, m_plugins);
    listerySetWindowFlag(&dialog, Qt::WindowStaysOnTopHint, true);
    connect(&dialog, &SettingsDialog::rescanRequested, this, [this]() { startScan(true); });

    if (dialog.exec() == QDialog::Accepted) {
        const bool extensionsChanged = previousExtensions != m_settings.executableExtensions();
        applySettings();
        // 插件的启用状态 / 热键可能改了：重新注册全局热键
        refreshPluginHotkeys();
        if (extensionsChanged && !(m_scanner && m_scanner->isRunning()))
            startScan(false, true); // 后缀变了要重新遍历，不能用 locate 快照
    }
}

void AppController::applySettings()
{
    if (m_window) {
        m_window->setMaxResults(m_settings.maxResults());
        m_window->setHideOnFocusLost(m_settings.hideWhenFocusLost());
    }

    if (m_hook)
        m_hook->setIntervalMs(m_settings.hotkeyIntervalMs());

    // 刷新间隔/智能调度改了：按新策略重新排下一次扫描
    scheduleNextScan();

    applyAutoStart();
}

void AppController::applyAutoStart()
{
    if (!Autostart::isSupported())
        return;

    QString error;
    if (m_settings.autoStart()) {
        // 每次启动都重写一次，保证 exe 被移动后注册表里的路径仍然正确
        if (!Autostart::setEnabled(true, &error))
            qWarning("Listery: 写入开机自启动失败：%s", qPrintable(error));
    } else if (Autostart::isEnabled()) {
        if (!Autostart::setEnabled(false, &error))
            qWarning("Listery: 取消开机自启动失败：%s", qPrintable(error));
    }
}
