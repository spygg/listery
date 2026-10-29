#include "SelfTest.h"

#include "Core/AppSettings.h"
#include "Core/Database.h"
#include "Core/Scanner.h"
#include "Core/SearchEngine.h"
#include "Platform/DoubleCtrlDetector.h"
#include "Platform/HotkeyHook.h"
#include "Platform/WindowUtils.h"
#include "Ui/LauncherWindow.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QScreen>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <QtGlobal>

#include <QDebug>

#include <cstdio>

namespace {
int g_failures = 0;

void check(bool condition, const QString &message)
{
    qInfo().noquote() << (condition ? QStringLiteral("[PASS]") : QStringLiteral("[FAIL]")) << message;
    if (!condition)
        ++g_failures;
}

void writeStub(const QString &path)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write("stub");
}

void addRecord(Database &database, const QString &path)
{
    ExecutableRecord record;
    record.path = QDir::toNativeSeparators(path);
    record.displayName = QFileInfo(path).fileName();
    record.dir = QDir::toNativeSeparators(QFileInfo(path).absolutePath());
    record.ext = QFileInfo(path).suffix().toLower();
    record.size = 1;
    record.mtime = 1;
    database.upsertExecutable(record, 777);
}
} // namespace

int SelfTest::run()
{
    QTemporaryDir tempDir;
    check(tempDir.isValid(), QStringLiteral("创建临时工作目录"));
    if (!tempDir.isValid())
        return 1;

    const QString root = tempDir.path();
    QDir().mkpath(root + QStringLiteral("/bin/nested"));

    writeStub(root + QStringLiteral("/bin/alpha.exe"));
    writeStub(root + QStringLiteral("/bin/beta.bat"));
    writeStub(root + QStringLiteral("/bin/gamma.sh"));
    writeStub(root + QStringLiteral("/bin/delta.txt"));
    writeStub(root + QStringLiteral("/bin/nested/epsilon.exe"));

    // ---- 1. 扫描与入库 ----
    const QString databasePath = root + QStringLiteral("/index.db");
    const QStringList extensions = QStringList() << QStringLiteral("exe")
                                                 << QStringLiteral("bat")
                                                 << QStringLiteral("sh");
    Scanner scanner(databasePath, extensions, QStringList(), false, 42,
                    QStringList() << root);
    scanner.start();
    const bool scannerFinished = scanner.wait(20000);
    check(scannerFinished, QStringLiteral("扫描线程完成（20s 内）"));

    Database database(databasePath);
    QString error;
    check(database.open(&error), QStringLiteral("打开索引库：%1").arg(error));
    check(database.totalCount() == 4,
          QStringLiteral("仅索引 exe/bat/sh，共 4 条，实际 %1 条").arg(database.totalCount()));
    check(!database.meta(QStringLiteral("last_scan_time")).isEmpty(),
          QStringLiteral("扫描时间写入元信息"));

    // ---- 2. 关键字检索与高亮 ----
    SearchEngine engine(&database);
    const QVector<SearchResult> alphaHits = engine.search(QStringLiteral("alpha"), 10);
    check(!alphaHits.isEmpty()
              && alphaHits.first().record.displayName.compare(QStringLiteral("alpha.exe"),
                                                              Qt::CaseInsensitive) == 0,
          QStringLiteral("关键字 alpha 命中 alpha.exe"));
    if (!alphaHits.isEmpty())
        check(alphaHits.first().highlights.size() == 5,
              QStringLiteral("高亮覆盖 5 个字符，实际 %1 个").arg(alphaHits.first().highlights.size()));

    check(engine.search(QStringLiteral("zzzzzz"), 10).isEmpty(),
          QStringLiteral("无关关键字返回空结果"));

    // ---- 3. 使用频次影响排序 ----
    const QString toolA = root + QStringLiteral("/bin/toolA.exe");
    const QString toolB = root + QStringLiteral("/bin/toolB.exe");
    writeStub(toolA);
    writeStub(toolB);
    database.beginBulk();
    addRecord(database, toolA);
    addRecord(database, toolB);
    database.commitBulk();

    for (int i = 0; i < 3; ++i)
        database.recordLaunch(toolB);

    const QVector<SearchResult> toolHits = engine.search(QStringLiteral("tool"), 10);
    check(toolHits.size() >= 2,
          QStringLiteral("关键字 tool 至少命中 2 条，实际 %1 条").arg(toolHits.size()));
    check(!toolHits.isEmpty()
              && toolHits.first().record.displayName.startsWith(QStringLiteral("toolB")),
          QStringLiteral("使用 3 次的 toolB 排在 toolA 前面"));

    // ---- 4. 空查询按最近使用 ----
    const QVector<SearchResult> recentHits = engine.search(QString(), 5);
    check(!recentHits.isEmpty(),
          QStringLiteral("空查询返回最近使用列表"));
    check(!recentHits.isEmpty()
              && recentHits.first().record.displayName.startsWith(QStringLiteral("toolB")),
          QStringLiteral("空查询首项为最近使用的 toolB"));

    // ---- 5. 数据目录与弹窗形态 ----
    const QString dataDir = AppSettings::resolveDataDir();
    check(QDir(dataDir).canonicalPath() == QDir(QCoreApplication::applicationDirPath()).canonicalPath(),
          QStringLiteral("数据目录与可执行文件同目录：%1").arg(QDir::toNativeSeparators(dataDir)));

    QWidget probe;
    probe.setAttribute(Qt::WA_DontShowOnScreen, true);
    probe.resize(760, 200);
    WindowUtils::placeCenterTop(&probe);
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect();
    const int expectedX = available.left() + (available.width() - probe.width()) / 2;
    const int expectedY = available.top() + qMax(48, available.height() * 12 / 100);
    check(qAbs(probe.x() - expectedX) <= 1 && qAbs(probe.y() - expectedY) <= 1,
          QStringLiteral("弹窗水平居中、垂直靠上（x=%1, y=%2）").arg(probe.x()).arg(probe.y()));

    LauncherWindow window(&engine, 10);
    window.setQueryForPreview(QStringLiteral("zzzzzz"));
    const int compactHeight = window.height();
    window.setQueryForPreview(QStringLiteral("alpha"));
    const int expandedHeight = window.height();
    window.setQueryForPreview(QString());
    const int inputOnlyHeight = window.height();
    check(compactHeight == 146,
          QStringLiteral("无候选时不展开列表，窗口高度 %1（期望 146）").arg(compactHeight));
    check(expandedHeight == 201,
          QStringLiteral("有候选时展开列表，窗口高度 %1（期望 201）").arg(expandedHeight));
    check(inputOnlyHeight == 110,
          QStringLiteral("空输入只保留输入框，窗口高度 %1（期望 110）").arg(inputOnlyHeight));

    SearchResult menuResult;
    menuResult.record.path = QStringLiteral("C:\\Windows\\System32\\cmd.exe");
    QAction *adminAction = nullptr;
    QAction *revealAction = nullptr;
    QAction *copyAction = nullptr;
    QMenu *contextMenu = window.createResultMenu(menuResult, &adminAction, &revealAction, &copyAction);
    const bool menuOk = contextMenu != nullptr
        && contextMenu->actions().size() == 3
        && adminAction != nullptr && revealAction != nullptr && copyAction != nullptr
        && adminAction->text() == QStringLiteral("以管理员身份运行")
        && revealAction->text() == QStringLiteral("打开所在位置")
        && copyAction->text() == QStringLiteral("复制路径到剪贴板");
    delete contextMenu;
    check(menuOk, QStringLiteral("结果右键菜单包含“管理员运行 / 打开位置 / 复制路径”三项"));

    // ---- 6. 双击 Ctrl 状态机 ----
    DoubleCtrlDetector detector;
    detector.setIntervalMs(300);

    check(!detector.handleKey(true, true, 1000), QStringLiteral("第一次按下 Ctrl 不触发"));
    detector.handleKey(true, false, 1010);
    check(detector.handleKey(true, true, 1200), QStringLiteral("300ms 内第二次按下触发"));

    detector.reset();
    detector.handleKey(true, true, 2000);
    detector.handleKey(true, false, 2010);
    check(!detector.handleKey(true, true, 2600), QStringLiteral("超过 300ms 不触发"));

    detector.reset();
    detector.handleKey(true, true, 3000);
    detector.handleKey(false, true, 3010);
    detector.handleKey(false, false, 3020);
    detector.handleKey(true, false, 3030);
    check(!detector.handleKey(true, true, 3100),
          QStringLiteral("中间夹带其它按键（Ctrl+C）不触发"));

    detector.reset();
    detector.handleKey(true, true, 4000);
    detector.handleKey(true, false, 4010);
    check(detector.handleKey(true, true, 4100), QStringLiteral("标准双击 Ctrl 触发"));
    detector.handleKey(true, false, 4110);
    check(!detector.handleKey(true, true, 4200), QStringLiteral("连续第三次按下不重复触发"));

    detector.reset();
    detector.handleKey(true, true, 5000);
    detector.handleKey(true, false, 5010);
    detector.handleKey(true, true, 5100);
    check(!detector.handleKey(true, true, 5110), QStringLiteral("按住不放产生的重复 keydown 不触发"));

    // ---- 7. 全局键盘钩子 ----
#ifdef Q_OS_WIN
    DoubleCtrlHook hook;
    hook.setIntervalMs(300);
    hook.start();
    QThread::msleep(500);
    const bool hookRunning = hook.isRunning();
    hook.requestStop();
    hook.wait(2000);
    check(hookRunning, QStringLiteral("WH_KEYBOARD_LL 全局钩子安装成功并保持运行"));
#else
    // 非 Windows 平台：全局钩子分别走 XRecord / CGEventTap，校验“可用性判断”与运行环境一致
    const bool supported = DoubleCtrlHook::isSupported();
#  if defined(Q_OS_LINUX)
    const bool expectSupported = !qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariable("XDG_SESSION_TYPE").compare(QStringLiteral("wayland"), Qt::CaseInsensitive) != 0;
    check(supported == expectSupported,
          QStringLiteral("Linux 全局钩子可用性与 DISPLAY/Wayland 判断一致（当前 %1）")
              .arg(supported ? QStringLiteral("可用") : QStringLiteral("不可用")));
#  else
    check(supported, QStringLiteral("macOS 全局钩子可用"));
#  endif
#endif

    qInfo().noquote() << QStringLiteral("自检结束：%1（失败 %2 项）")
                             .arg(g_failures == 0 ? QStringLiteral("全部通过") : QStringLiteral("存在失败"))
                             .arg(g_failures);
    return g_failures == 0 ? 0 : 1;
}
