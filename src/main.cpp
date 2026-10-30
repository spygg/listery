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
        if (g_logFile && g_logFile->isOpen()) {
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
 * 日志轮转：只保留最近 keepDays 天的记录，更早的整行丢弃。
 * 日志行形如 "[2026-10-30T05:41:14.123] [INFO] ..."，按首部时间戳的日期判断。
 * 无法解析日期的行一律保留，避免误删；没有过期行时不做任何重写（零开销）。
 */
void trimLog(const QString &path, int keepDays)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    const QByteArray content = file.readAll();
    file.close();

    const QStringList lines = QString::fromUtf8(content).split(QLatin1Char('\n'),
                                                              LISTERY_SKIP_EMPTY_PARTS);
    if (lines.isEmpty())
        return;

    const QDate cutoff = QDate::currentDate().addDays(-keepDays);
    int firstKeep = 0;
    bool scanning = true;
    for (int i = 0; i < lines.size() && scanning; ++i) {
        const QString &line = lines.at(i);
        const int open = line.indexOf(QLatin1Char('['));
        const int close = line.indexOf(QLatin1Char(']'), open + 1);
        if (open < 0 || close < 0)
            break; // 无法解析：保留本行及之后全部
        const QString ts = line.mid(open + 1, close - open - 1);
        const QString datePart = ts.left(ts.indexOf(QLatin1Char('T')));
        const QDate date = QDate::fromString(datePart, QStringLiteral("yyyy-MM-dd"));
        if (date.isValid() && date < cutoff)
            firstKeep = i + 1; // 该行已过期，从下一行起保留
        else
            scanning = false;  // 遇到不旧的行，其后全部保留
    }

    if (firstKeep <= 0)
        return; // 没有需要丢弃的旧记录

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return;
    for (int i = firstKeep; i < lines.size(); ++i) {
        out.write(lines.at(i).toUtf8());
        out.write("\n");
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
    const QString logPath = dataDir + QStringLiteral("/listery.log");
    // 启动前先按 3 天窗口裁剪，避免日志无限增长
    trimLog(logPath, 3);
    g_logFile = new QFile(logPath);
    g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
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
