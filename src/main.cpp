#include "AppController.h"

#include "Core/QtCompat.h"
#include "Core/AppSettings.h"
#include "Core/Database.h"
#include "Core/SearchService.h"
#include "Core/Updater.h"
#include "Platform/Branding.h"
#include "Platform/WindowUtils.h"
#include "SelfTest.h"
#include "Ui/LauncherWindow.h"

#ifdef LISTERY_WITH_BREAKPAD
#  include "crashhandler.h"
#endif

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QLocalSocket>
#include <QMessageBox>
#include <QMutex>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace {
QFile *g_logFile = nullptr;
QString g_logDir;
QString g_logDate;
/** 日志可能来自 GUI 线程、扫描线程、插件回调：QFile 不是线程安全的，写之前必须串行化。 */
QMutex g_logMutex;

void logMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Q_UNUSED(context)
    const QString level = (type == QtDebugMsg)      ? QStringLiteral("DEBUG")
                        : (type == QtInfoMsg)       ? QStringLiteral("INFO")
                        : (type == QtWarningMsg)    ? QStringLiteral("WARN")
                        : (type == QtCriticalMsg)   ? QStringLiteral("ERROR")
                                                    : QStringLiteral("FATAL");
    const QString line = QStringLiteral("[%1] [%2] %3\n")
                             .arg(listeryIsoTimestamp(), level, message);

    {
        QMutexLocker locker(&g_logMutex);
        // 按天分文件：跨过零点后第一条日志自动切到新日期，长时间挂着运行也不会写错文件
        const QString today = QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
        if (!g_logFile || !g_logFile->isOpen() || g_logDate != today) {
            if (g_logFile && g_logFile->isOpen()) {
                g_logFile->flush();
                g_logFile->close();
            }
            if (!g_logFile)
                g_logFile = new QFile;
            g_logFile->setFileName(g_logDir + QLatin1Char('/') + today + QStringLiteral(".txt"));
            if (g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
                g_logDate = today;
            else
                g_logDate.clear();
        }

        if (g_logFile->isOpen()) {
            g_logFile->write(line.toUtf8());
            g_logFile->flush();
            if (type != QtFatalMsg)
                return;
        } else {
            std::fputs(qPrintable(line), stderr);
        }
    }

    // qFatal 的语义是“记完这条就结束进程”：不 abort 会让程序带着已损坏的状态继续跑
    if (type == QtFatalMsg)
        std::abort();
}

/**
 * 日志按天分文件（logs/YYYYMMDD.txt），启动时只清理保留窗口外的旧文件。
 * 相比“单个文件 + 启动时重写裁剪”，省掉每次启动读入并重写整份日志的开销，
 * 文件也不会越写越大。
 */
void pruneLogs(const QString &dir, int keepDays)
{
    const QDate cutoff = QDate::currentDate().addDays(-(keepDays - 1));
    const QDir logs(dir);
    const QStringList names = logs.entryList(QStringList() << QStringLiteral("*.txt"), QDir::Files);
    for (const QString &name : names) {
        const QDate date = QDate::fromString(QFileInfo(name).completeBaseName(),
                                             QStringLiteral("yyyyMMdd"));
        if (date.isValid() && date < cutoff)
            QFile::remove(logs.filePath(name));
    }
}

/** 离屏渲染界面截图，用于文档配图与回归检查（不弹出任何窗口）。 */
int renderPreview(const QString &outputPath, const QString &query)
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid())
        return 2;

    const QString previewDatabasePath = tempDir.path() + QStringLiteral("/preview.db");
    Database database(previewDatabasePath);
    QString error;
    if (!database.open(&error)) {
        qWarning("preview: 无法打开预览库：%s", qPrintable(error));
        return 3;
    }

    struct PreviewItem
    {
        const char *path;
        int uses;
    };

    const PreviewItem items[] = {
        {"C:/Windows/System32/notepad.exe", 12},
        {"C:/Windows/System32/cmd.exe", 9},
        {"C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe", 8},
        {"C:/Windows/System32/mspaint.exe", 6},
        {"E:/programes/Qt/Qt/Tools/mingw810_64/bin/g++.exe", 4},
        {"E:/programes/Qt/Qt/Tools/CMake_64/bin/cmake.exe", 3},
        {"C:/Windows/System32/curl.exe", 2},
        {"C:/Windows/System32/taskmgr.exe", 1},
    };

    database.beginBulk();
    for (const PreviewItem &item : items) {
        const QFileInfo info(QString::fromUtf8(item.path));
        if (!info.exists())
            continue;

        ExecutableRecord record;
        record.path = QDir::toNativeSeparators(info.absoluteFilePath());
        record.displayName = info.fileName();
        record.dir = QDir::toNativeSeparators(info.absolutePath());
        record.ext = info.suffix().toLower();
        record.size = info.size();
        record.mtime = info.lastModified().toMSecsSinceEpoch();
        database.upsertExecutable(record, 1);

        for (int i = 0; i < item.uses; ++i)
            database.recordLaunch(record.path);
    }
    database.commitBulk();

    SearchService service(previewDatabasePath);
    QString searchError;
    if (!service.start(&searchError)) {
        qWarning("preview: 无法启动检索线程：%s", qPrintable(searchError));
        return 6;
    }

    LauncherWindow window(&service, 10);
    window.setStatusText(QStringLiteral("已索引 128,436 个可执行文件 · 更新于 10-29 21:00"));

    // 只做逻辑上的“显示”，不真正映射到屏幕，避免打扰当前桌面
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.show();
    window.setQueryForPreview(query);
    if (!window.waitForSearch(3000))
        qWarning("preview: 等待检索结果超时");
    QApplication::processEvents();

    const QPixmap shot = window.grab();
    if (shot.isNull())
        return 4;

    QImage canvas(shot.width() + 64, shot.height() + 64, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor(0xED, 0xF0, 0xF4));
    QPainter painter(&canvas);
    painter.drawPixmap(32, 32, shot);
    painter.end();

    if (!canvas.save(outputPath))
        return 5;

    qInfo("preview: 已保存 %s (%dx%d)", qPrintable(outputPath), canvas.width(), canvas.height());
    return 0;
}

#ifdef Q_OS_WIN
/** 模拟“双击 Ctrl”：按下-抬起-按下-抬起，两步间隔远小于 300ms。 */
void sendDoubleCtrl()
{
    INPUT inputs[4];
    ZeroMemory(inputs, sizeof(inputs));
    for (int i = 0; i < 4; ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = VK_LCONTROL;
        inputs[i].ki.dwFlags = (i % 2 == 0) ? 0 : KEYEVENTF_KEYUP;
    }
    SendInput(4, inputs, sizeof(INPUT));
}
#endif
} // namespace

int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Listery"));
    app.setApplicationDisplayName(QStringLiteral("Listery"));
    app.setOrganizationName(QStringLiteral("Listery"));
    // 版本号统一由 CMake 注入的 LISTERY_VERSION 决定（自动更新也读这一个），
    // 之前这里写死 "1.0.0"，和发布版本对不上
    app.setApplicationVersion(Updater::currentVersion());
    app.setQuitOnLastWindowClosed(false);

    WindowUtils::setAppUserModelId(QStringLiteral("Listery.Spotlight.Desktop"));

    const QString dataDir = AppSettings::resolveDataDir();
    QDir().mkpath(dataDir);

#ifdef LISTERY_WITH_BREAKPAD
    // 崩溃转储写到 dataDir/crashes，仅本地保留，不做任何自动上传
    const QString crashDir = dataDir + QStringLiteral("/crashes");
    QDir().mkpath(crashDir);
    Breakpad::CrashHandler::instance()->Init(crashDir);
#endif

    // 日志按天落到 dataDir/logs/YYYYMMDD.txt，只保留最近 7 个自然日
    const QString logDir = dataDir + QStringLiteral("/logs");
    QDir().mkpath(logDir);
    pruneLogs(logDir, 7);
    g_logDir = logDir;
    qInstallMessageHandler(logMessageHandler);

    QFont appFont = app.font();
#ifdef Q_OS_WIN
    appFont.setFamily(QStringLiteral("Microsoft YaHei UI"));
#endif
    appFont.setPointSize(10);
    app.setFont(appFont);

    app.setWindowIcon(Branding::appIcon());

    const QStringList arguments = app.arguments();

    const int previewIndex = arguments.indexOf(QStringLiteral("--render-preview"));
    if (previewIndex >= 0 && previewIndex + 1 < arguments.size()) {
        QString query = (previewIndex + 2 < arguments.size())
            ? arguments.at(previewIndex + 2)
            : QStringLiteral("c");
        if (query == QLatin1String("--empty"))
            query.clear();
        const int result = renderPreview(arguments.at(previewIndex + 1), query);
        if (g_logFile)
            g_logFile->flush();
        return result;
    }

    if (arguments.contains(QStringLiteral("--selftest"))) {
        const int result = SelfTest::run();
        if (g_logFile)
            g_logFile->flush();
        return result;
    }

    // 单实例：已有实例在跑就把“唤起”消息发过去并退出
    QLocalSocket probe;
    probe.connectToServer(QString::fromLatin1(AppConstants::kSingleInstanceKey));
    if (probe.waitForConnected(500)) {
        probe.write("show");
        probe.waitForBytesWritten(500);
        return 0;
    }

    const bool testHotkey = arguments.contains(QStringLiteral("--test-hotkey"));
    if (testHotkey)
        qputenv("LISTERY_TEST_ALLOW_INJECTED", "1");

    AppController controller;
    controller.setSilentStart(arguments.contains(QStringLiteral("--autostart")));
    if (!controller.initialize())
        return 1;

    if (testHotkey) {
#ifdef Q_OS_WIN
        QTimer::singleShot(1200, &controller, []() { sendDoubleCtrl(); });
        QTimer::singleShot(2400, &controller, [&controller]() {
            const bool visible = controller.isLauncherVisible();
            qInfo("test-hotkey: launcherVisible=%d", visible ? 1 : 0);
            QApplication::exit(visible ? 0 : 1);
        });
#else
        qInfo("test-hotkey: 当前平台未实现全局热键");
        return 2;
#endif
    }

    if (app.arguments().contains(QStringLiteral("--show"))) {
        QTimer::singleShot(0, &controller, [&controller]() {
            controller.showLauncher();
        });
    }

    if (app.arguments().contains(QStringLiteral("--rescan"))) {
        QTimer::singleShot(0, &controller, [&controller]() {
            controller.startScan(true);
        });
    }

    const int exitCode = app.exec();
    // 退出时把日志句柄关掉并释放：否则 QFile 及其缓冲要到进程结束才回收，
    // 调试器/文件监控也会一直认为日志被占用
    {
        QMutexLocker locker(&g_logMutex);
        if (g_logFile) {
            g_logFile->flush();
            g_logFile->close();
            delete g_logFile;
            g_logFile = nullptr;
        }
    }
    return exitCode;
}
