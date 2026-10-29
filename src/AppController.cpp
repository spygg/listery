#include "AppController.h"

#include "Core/LaunchService.h"
#include "Core/Scanner.h"
#include "Core/SearchEngine.h"
#include "Platform/Autostart.h"
#include "Platform/Branding.h"
#include "Platform/HotkeyHook.h"
#include "Ui/LauncherWindow.h"
#include "Ui/SettingsDialog.h"

#include "Core/UsageRecorder.h"

#include <QAction>
#include <QApplication>
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

#include <algorithm>

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
    m_database->setMeta(QStringLiteral("app_version"), QStringLiteral("1.0.0"));

    m_searchEngine.reset(new SearchEngine(m_database.get()));

    // 使用统计写入器：独立后台线程 + 独立连接，不占用 GUI 线程
    m_usageThread = new QThread(this);
    m_usage = new UsageRecorder(m_settings.databasePath());
    m_usage->moveToThread(m_usageThread);
    connect(m_usageThread, &QThread::finished, m_usage, &UsageRecorder::shutdown);
    connect(m_usageThread, &QThread::finished, m_usage, &QObject::deleteLater);
    m_usageThread->start();

    m_window = new LauncherWindow(m_searchEngine.get(), m_settings.maxResults());
    m_window->setHideOnFocusLost(m_settings.hideWhenFocusLost());
    m_window->setStatusText(QStringLiteral("索引尚未建立"));

    createTrayIcon();
    setupSingleInstance();
    setupHotkey();
    setupScanSchedule();
    setupConnections();

    const int count = indexCount();
    if (count == 0) {
        m_window->setStatusText(QStringLiteral("索引尚未建立 · 首次扫描进行中"));
        if (!m_silentStart)
            QTimer::singleShot(600, this, &AppController::showLauncher);
    }
    updateStatus();
    applyAutoStart();

    if (count == 0 || m_settings.autoScanOnStart()) {
        QTimer::singleShot(count == 0 ? 300 : 1500, this, [this, count]() {
            startScan(count == 0);
        });
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
}

void AppController::createTrayIcon()
{
    m_tray = new QSystemTrayIcon(Branding::appIcon(), this);
    m_tray->setToolTip(QStringLiteral("Listery · 双击 Ctrl 唤起搜索"));

    auto *menu = new QMenu();
    m_actionShow = menu->addAction(QStringLiteral("打开搜索框"));
    m_actionRescan = menu->addAction(QStringLiteral("重新扫描全盘"));
    m_actionSettings = menu->addAction(QStringLiteral("设置…"));
    menu->addSeparator();
    m_actionQuit = menu->addAction(QStringLiteral("退出"));

    connect(m_actionShow, &QAction::triggered, this, &AppController::showLauncher);
    connect(m_actionRescan, &QAction::triggered, this, [this]() { startScan(true); });
    connect(m_actionSettings, &QAction::triggered, this, &AppController::openSettings);
    connect(m_actionQuit, &QAction::triggered, qApp, &QApplication::quit);

    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    showLauncher();
            });
    m_tray->show();
}

void AppController::setupSingleInstance()
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
        // 拿不到单实例锁通常说明已有实例在运行，此时保持静默，避免反复弹窗
        qWarning("Listery: 单实例监听失败：%s", qPrintable(m_server->errorString()));
    }
}

void AppController::setupHotkey()
{
    if (!DoubleCtrlHook::isSupported()) {
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

    m_hook = new DoubleCtrlHook(this);
    m_hook->setIntervalMs(m_settings.hotkeyIntervalMs());
    connect(m_hook, &DoubleCtrlHook::activated, this, &AppController::showLauncher);
    connect(m_hook, &DoubleCtrlHook::failed, this, [this](const QString &reason) {
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
    connect(m_scanTimer, &QTimer::timeout, this, [this]() { startScan(false); });

    if (m_settings.rescanIntervalMinutes() > 0) {
        const qint64 interval = static_cast<qint64>(m_settings.rescanIntervalMinutes()) * 60 * 1000;
        m_scanTimer->start(static_cast<int>(qMin<qint64>(interval, 2147483647LL)));
    }
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

void AppController::startScan(bool notifyUser)
{
    if (!m_database)
        return;

    if (m_scanner && m_scanner->isRunning()) {
        m_scanPending = true;
        if (notifyUser && m_tray)
            m_tray->showMessage(QStringLiteral("Listery"),
                                QStringLiteral("已有扫描在进行，完成后会自动再扫一遍"),
                                QSystemTrayIcon::Information, 3000);
        return;
    }

    if (m_scanner) {
        m_scanner->requestStop();
        m_scanner->wait(2500);
        m_scanner->deleteLater();
        m_scanner = nullptr;
    }

    const qint64 generation = QDateTime::currentMSecsSinceEpoch();
    m_scanNotify = notifyUser;

    m_scanner = new Scanner(m_settings.databasePath(),
                            m_settings.executableExtensions(),
                            m_settings.skipDirectoryPatterns(),
                            m_settings.includeRemovableDrives(),
                            generation,
                            QStringList(),
                            this);
    connect(m_scanner, &Scanner::progressChanged, this, &AppController::onScanProgress);
    connect(m_scanner, &Scanner::scanFinished, this, &AppController::onScanFinished);
    m_scanner->start();

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
    if (m_scanner) {
        m_scanner->deleteLater();
        m_scanner = nullptr;
    }

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
        QTimer::singleShot(200, this, [this]() { startScan(true); });
    }
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

    SettingsDialog dialog(&m_settings);
    dialog.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    connect(&dialog, &SettingsDialog::rescanRequested, this, [this]() { startScan(true); });

    if (dialog.exec() == QDialog::Accepted) {
        const bool extensionsChanged = previousExtensions != m_settings.executableExtensions();
        applySettings();
        if (extensionsChanged && !(m_scanner && m_scanner->isRunning()))
            startScan(false);
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

    if (m_scanTimer) {
        m_scanTimer->stop();
        if (m_settings.rescanIntervalMinutes() > 0) {
            const qint64 interval = static_cast<qint64>(m_settings.rescanIntervalMinutes()) * 60 * 1000;
            m_scanTimer->start(static_cast<int>(qMin<qint64>(interval, 2147483647LL)));
        }
    }

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
