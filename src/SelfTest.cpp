#include "SelfTest.h"

#include "AppController.h"

#include "Core/AppSettings.h"
#include "Core/Database.h"
#include "Core/PluginHost.h"
#include "Core/Pinyin.h"
#include "Core/Scanner.h"
#include "Core/SearchEngine.h"
#include "Core/SearchService.h"
#include "Core/Updater.h"
#include "Platform/DoubleCtrlDetector.h"
#include "Platform/HotkeyHook.h"
#include "Platform/WindowUtils.h"
#include "Core/QtCompat.h"
#include "Ui/LauncherWindow.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QScreen>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
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

bool writeStub(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("Listery: 写存根失败 %s：%s", qPrintable(path), qPrintable(file.errorString()));
        return false;
    }
    if (file.write("stub") <= 0) {
        qWarning("Listery: 写存根无字节写入 %s", qPrintable(path));
        return false;
    }
    return true;
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
    const QString lnkStub = root + QStringLiteral("/bin/腾讯会议.lnk");
    // 沙箱/杀软可能会拦截 .lnk 的创建（open 成功但文件不落地），落不了盘就退回到直接写记录
    const bool lnkStubOnDisk = writeStub(lnkStub) && QFileInfo::exists(lnkStub);
    if (!lnkStubOnDisk)
        qInfo().noquote() << QStringLiteral("[INFO] 当前环境不允许创建 .lnk 存根，改为直接写入索引记录");

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

    if (!lnkStubOnDisk) {
        ExecutableRecord shortcut;
        shortcut.path = QDir::toNativeSeparators(lnkStub);
        shortcut.displayName = QStringLiteral("腾讯会议");
        shortcut.dir = QDir::toNativeSeparators(QFileInfo(lnkStub).absolutePath());
        shortcut.ext = QStringLiteral("lnk");
        shortcut.size = 4;
        shortcut.mtime = 1;
        Pinyin::buildSearchForms(shortcut.displayName, &shortcut.pinyin,
                                 &shortcut.pinyinInitials);
        database.beginBulk();
        database.upsertExecutable(shortcut, 42);
        database.commitBulk();
    }

    // exe/bat/sh 共 4 条 + .lnk 快捷方式 1 条（显示名去掉扩展名由扫描器负责）
    check(database.totalCount() == 5,
          QStringLiteral("索引 exe/bat/sh 与 lnk 快捷方式，共 5 条，实际 %1 条")
              .arg(database.totalCount()));
    check(!database.meta(QStringLiteral("last_scan_time")).isEmpty(),
          QStringLiteral("扫描时间写入元信息"));

    // ---- 拼音转换：全拼 / 首字母 / 多音字 ----
    QString fullPinyin;
    QString initials;
    Pinyin::buildSearchForms(QStringLiteral("腾讯会议.lnk"), &fullPinyin, &initials);
    check(fullPinyin.contains(QStringLiteral("tengxunhuiyi"))
              && initials.contains(QStringLiteral("txhy")),
          QStringLiteral("腾讯会议 生成全拼与首字母：%1 / %2").arg(fullPinyin, initials));

    Pinyin::buildSearchForms(QStringLiteral("招商银行"), &fullPinyin, &initials);
    check(fullPinyin.contains(QStringLiteral("zhaoshangyinhang")),
          QStringLiteral("多音字“行”保留 yinhang 读音：%1").arg(fullPinyin));

    Pinyin::buildSearchForms(QStringLiteral("chrome.exe"), &fullPinyin, &initials);
    check(fullPinyin.isEmpty() && initials.isEmpty(),
          QStringLiteral("纯英文名不生成拼音"));

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

    // ---- 中文名：汉字、全拼、首字母三种输入都要命中同一快捷方式 ----
    const QVector<SearchResult> hanziHits = engine.search(QStringLiteral("腾讯会议"), 10);
    check(!hanziHits.isEmpty()
              && hanziHits.first().record.displayName == QStringLiteral("腾讯会议"),
          QStringLiteral("汉字输入命中腾讯会议快捷方式"));

    const QVector<SearchResult> pinyinHits = engine.search(QStringLiteral("tengxunhuiyi"), 10);
    check(!pinyinHits.isEmpty()
              && pinyinHits.first().record.displayName == QStringLiteral("腾讯会议"),
          QStringLiteral("全拼 tengxunhuiyi 命中腾讯会议快捷方式"));

    const QVector<SearchResult> initialsHits = engine.search(QStringLiteral("txhy"), 10);
    check(!initialsHits.isEmpty()
              && initialsHits.first().record.displayName == QStringLiteral("腾讯会议"),
          QStringLiteral("首字母 txhy 命中腾讯会议快捷方式"));

    const QVector<SearchResult> oneCharHits = engine.search(QStringLiteral("a"), 10);
    check(!oneCharHits.isEmpty()
              && oneCharHits.first().record.displayName.compare(QStringLiteral("alpha.exe"),
                                                               Qt::CaseInsensitive) == 0,
          QStringLiteral("单字符查询走前缀匹配，命中 alpha.exe"));

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

    // ---- FTS5 倒排索引随增量写入同步 ----
    if (database.ftsAvailable()) {
        const QVector<ExecutableRecord> ftsHits =
            database.searchByFts(QStringList() << QStringLiteral("toolb"), 10, false);
        check(!ftsHits.isEmpty(),
              QStringLiteral("FTS5 索引随扫描与增量写入同步（toolb 命中 %1 条）").arg(ftsHits.size()));
    } else {
        qInfo().noquote() << QStringLiteral("[INFO] 当前 SQLite 未启用 FTS5，检索自动退化为 LIKE 扫描");
    }

    // ---- 含扩展名的查询必须命中：detail=column 下短语查询会直接报错 ----
    if (database.ftsAvailable()) {
        check(!database.searchByFts(QStringList() << QStringLiteral("alpha.exe"), 10, true).isEmpty(),
              QStringLiteral("含扩展名的 FTS 查询命中 alpha.exe"));
    }

    // ---- 使用率种子：rowid 靠后的常用程序也要能进入候选 ----
    const QVector<ExecutableRecord> seedHits =
        database.searchUsageSeeds(QStringList() << QStringLiteral("tool"), 10);
    check(!seedHits.isEmpty()
              && QFileInfo(seedHits.first().path).fileName().startsWith(QStringLiteral("toolB")),
          QStringLiteral("使用率种子把常用程序排在最前（%1 条）").arg(seedHits.size()));

    const QHash<QString, UsageStat> snapshot = database.usageSnapshot();
    check(snapshot.value(QDir::toNativeSeparators(toolB)).launchCount == 3,
          QStringLiteral("使用统计快照与写入一致"));

    const QVector<SearchResult> fullNameHits = engine.search(QStringLiteral("toolb.exe"), 10);
    check(!fullNameHits.isEmpty()
              && fullNameHits.first().record.displayName.startsWith(QStringLiteral("toolB")),
          QStringLiteral("输入完整文件名 toolb.exe 精确命中"));

    // ---- 卷级别统计：智能调度据此决定下一次什么时候再扫 ----
    const int expectedVolumeCount = 4 + (lnkStubOnDisk ? 1 : 0);
    const VolumeStat rootStat = database.volumeStat(root);
    check(rootStat.exeCount == expectedVolumeCount && rootStat.lastScan > 0,
          QStringLiteral("扫描后写入卷统计：%1 条（期望 %2）")
              .arg(rootStat.exeCount).arg(expectedVolumeCount));
    check(database.countInVolume(root) == database.totalCount(),
          QStringLiteral("按路径前缀统计卷内条目：%1 条").arg(database.countInVolume(root)));

    // 只扫一个“从没装过软件”的卷：它自己的统计要记为连续空扫，其它卷的记录不能被清理
    const int beforePartial = database.totalCount();
    const QString emptyVolume = root + QStringLiteral("/other/empty-bin");
    QDir().mkpath(emptyVolume);
    Scanner emptyScanner(databasePath, extensions, QStringList(), false, 43,
                         QStringList() << emptyVolume);
    emptyScanner.start();
    check(emptyScanner.wait(20000), QStringLiteral("只扫单个卷的扫描线程完成"));

    const VolumeStat emptyStat = database.volumeStat(emptyVolume);
    check(emptyStat.exeCount == 0 && emptyStat.emptyRounds == 1 && emptyStat.lastScan > 0,
          QStringLiteral("空卷记录为连续 0 条（rounds=%1）").arg(emptyStat.emptyRounds));
    check(database.totalCount() == beforePartial,
          QStringLiteral("只扫空卷时其它卷的 %1 条记录仍然保留（扫描前 %2 条）")
              .arg(database.totalCount()).arg(beforePartial));

    // ---- 智能调度的间隔策略 ----
    const qint64 base = 720LL * 60 * 1000; // 12 小时
    VolumeStat busy;
    busy.exeCount = 1200;
    check(AppController::scanIntervalForVolume(busy, base, true) == base,
          QStringLiteral("装满程序的卷按用户配置的间隔刷新"));

    VolumeStat few;
    few.exeCount = 3;
    check(AppController::scanIntervalForVolume(few, base, true) == base * 2,
          QStringLiteral("只有零星几个程序的卷放宽到 2 倍"));

    VolumeStat none1;
    none1.exeCount = 0;
    none1.emptyRounds = 1;
    check(AppController::scanIntervalForVolume(none1, base, true) == base * 4,
          QStringLiteral("空卷第一次放宽到 4 倍（2 天）"));

    VolumeStat none3 = none1;
    none3.emptyRounds = 3;
    check(AppController::scanIntervalForVolume(none3, base, true) == base * 16,
          QStringLiteral("连续空扫 3 轮放宽到 16 倍（8 天）"));

    VolumeStat none9 = none1;
    none9.emptyRounds = 9;
    check(AppController::scanIntervalForVolume(none9, base, true) <= 14LL * 24 * 60 * 60 * 1000,
          QStringLiteral("再空扫也不会超过 14 天"));

    check(AppController::scanIntervalForVolume(none9, base, false) == base,
          QStringLiteral("关闭智能调度后所有卷一视同仁"));

    // ---- 扫描元信息 ----
    check(database.meta(QStringLiteral("scan_in_progress")).isEmpty(),
          QStringLiteral("扫描完成后清除“扫描进行中”标记"));
    check(database.meta(QStringLiteral("last_scan_error")).isEmpty(),
          QStringLiteral("成功扫描后清空上次错误"));

    // ---- 6. 拼音首字母前缀检索：输入 "wx" 必须命中 "微信" ----
    // 这是用户真实反馈的问题：开始菜单里的微信快捷方式（displayName=微信）搜不到。
    // 关键：searchByPrefix / searchByTokens 都要纳入拼音列，无论 FTS5 是否可用。
    {
        QTemporaryDir wxDir;
        const QString wxRoot = wxDir.path();
        QDir().mkpath(wxRoot + QStringLiteral("/bin"));
        const QString wechatLnk = wxRoot + QStringLiteral("/bin/微信.lnk");
        const bool onDisk = writeStub(wechatLnk);
        const QString wxDbPath = wxRoot + QStringLiteral("/wx.db");
        const QStringList wxExts = QStringList() << QStringLiteral("exe");

        {
            Scanner wxScanner(wxDbPath, wxExts, QStringList(), false, 7,
                              QStringList() << wxRoot);
            QObject::connect(&wxScanner, &Scanner::scanFinished,
                             &wxScanner, [](int found, int, bool canceled, QString err) {
                                 qInfo().noquote() << QStringLiteral("[DIAG] scanFinished found=")
                                                  << found << "canceled=" << canceled
                                                  << "err=" << err;
                             });
            wxScanner.start();
            check(wxScanner.wait(20000), QStringLiteral("wx 测试扫描线程完成"));
        }

        // 与主测试一致：open+write 可能成功但文件被沙箱/杀软拦截落不了盘，
        // 必须再用 exists 兜底判定；落不了盘时直接写一条带拼音的记录，验证检索路径本身。
        // （用户真实机器上 .lnk 能正常落盘，扫描器会把它收进索引，这里只是测试环境的降级。）
        const bool wechatOnDisk = onDisk && QFileInfo::exists(wechatLnk);

        if (!wechatOnDisk) {
            // .lnk 被沙箱拦截落不了盘时，直接写一条带拼音的记录，验证检索路径本身
            Database altDb(wxDbPath);
            QString e;
            check(altDb.open(&e), QStringLiteral("打开 wx 兜底库：%1").arg(e));
            ExecutableRecord rec;
            rec.path = QDir::toNativeSeparators(wechatLnk);
            rec.displayName = QStringLiteral("微信");
            rec.dir = QDir::toNativeSeparators(QFileInfo(wechatLnk).absolutePath());
            rec.ext = QStringLiteral("lnk");
            rec.size = 4;
            rec.mtime = 1;
            Pinyin::buildSearchForms(rec.displayName, &rec.pinyin, &rec.pinyinInitials);
            altDb.beginBulk();
            altDb.upsertExecutable(rec, 7);
            altDb.commitBulk();
        }

        Database wxDb(wxDbPath);
        QString wxErr;
        check(wxDb.open(&wxErr), QStringLiteral("打开 wx 测试库：%1").arg(wxErr));
        SearchEngine wxEngine(&wxDb);

        // 直接验证前缀检索覆盖拼音首字母（无需 FTS 也能命中）
        const QVector<ExecutableRecord> prefixHits = wxDb.searchByPrefix(QStringLiteral("wx"), 10);
        bool prefixOk = false;
        for (const ExecutableRecord &r : prefixHits)
            if (r.displayName == QStringLiteral("微信"))
                prefixOk = true;

        // 诊断：record 是否在库、拼音前缀能否命中
        qInfo().noquote() << QStringLiteral("[DIAG] wechatOnDisk=") << wechatOnDisk
                         << "total=" << wxDb.totalCount();
        const QVector<ExecutableRecord> byName = wxDb.searchByPrefix(QStringLiteral("微信"), 10);
        qInfo().noquote() << QStringLiteral("[DIAG] searchByPrefix(\"微信\") ->")
                         << byName.size() << "rows";
        for (const ExecutableRecord &r : byName)
            qInfo().noquote() << QStringLiteral("[DIAG]   ") << r.displayName
                             << "py=" << r.pinyin << "ini=" << r.pinyinInitials;
        qInfo().noquote() << QStringLiteral("[DIAG] searchByPrefix(\"wx\") ->")
                         << prefixHits.size() << "rows";

        check(prefixOk, QStringLiteral("searchByPrefix(\"wx\") 命中微信（拼音首字母路径）"));

        // 完整检索链路：与用户实际输入一致
        const QVector<SearchResult> hits = wxEngine.search(QStringLiteral("wx"), 10);
        bool ok = false;
        for (const SearchResult &r : hits)
            if (r.record.displayName == QStringLiteral("微信"))
                ok = true;
        check(ok, QStringLiteral("输入 wx 命中微信（完整检索链路）"));
    }

    // ---- 7. 版本比较：语义化大小比较，避开 1.2.0 被误判大于 1.10.0 的坑 ----
    {
        check(Updater::compareVersions(QStringLiteral("1.2.0"), QStringLiteral("1.10.0")) == -1,
              QStringLiteral("1.2.0 < 1.10.0（逐段比较）"));
        check(Updater::compareVersions(QStringLiteral("1.10.0"), QStringLiteral("1.2.0")) == 1,
              QStringLiteral("1.10.0 > 1.2.0"));
        check(Updater::compareVersions(QStringLiteral("1.2.0"), QStringLiteral("1.2.0")) == 0,
              QStringLiteral("相等版本返回 0"));
        check(Updater::compareVersions(QStringLiteral("2.0.0"), QStringLiteral("1.9.9")) == 1,
              QStringLiteral("主版本号优先：2.0.0 > 1.9.9"));
        check(Updater::compareVersions(QStringLiteral("1.2.0"), QStringLiteral("1.2")) == 0,
              QStringLiteral("缺省低位的版本视作 0"));
        check(Updater::stripTag(QStringLiteral("v1.2.0")) == QStringLiteral("1.2.0"),
              QStringLiteral("去掉版本号前缀 v"));
        check(Updater::compareVersions(Updater::currentVersion(), QStringLiteral("0.0.1")) >= 0,
              QStringLiteral("当前版本 %1 不低于 0.0.1").arg(Updater::currentVersion()));
    }

    // ---- 8. AppData 隐藏目录扫描：用户级应用（豆包等）装在 AppData\Local，
    //        这些目录是隐藏目录，全盘遍历进不去、开始菜单又没它们的快捷方式，
    //        必须通过 userAppDataRoots() 显式扫描。这是用户真实反馈“搜 doublao 找不到豆包”的根因。
    {
        QTemporaryDir appDataDir;
        const QString appDataRoot = appDataDir.path();
        // 模拟 C:\Users\<user>\AppData\Local\Doubao\Application\Doubao.exe
        const QString doubaoPath = appDataRoot
            + QStringLiteral("/Doubao/Application/Doubao.exe");
        QDir().mkpath(QFileInfo(doubaoPath).absolutePath());
        check(writeStub(doubaoPath), QStringLiteral("写入模拟豆包可执行文件"));

        const QString adDbPath = appDataDir.path() + QStringLiteral("/ad.db");
        const QStringList adExts = QStringList() << QStringLiteral("exe");
        Scanner adScanner(adDbPath, adExts, QStringList(), false, 11,
                          QStringList() << appDataRoot);
        adScanner.start();
        check(adScanner.wait(20000), QStringLiteral("AppData 模拟扫描线程完成"));

        Database adDb(adDbPath);
        QString adErr;
        check(adDb.open(&adErr), QStringLiteral("打开 AppData 测试库：%1").arg(adErr));
        SearchEngine adEngine(&adDb);

        // 完整检索链路：与用户实际输入一致，“doubao” 必须命中深藏在 AppData 下的 Doubao.exe
        const QVector<SearchResult> doubaoHits = adEngine.search(QStringLiteral("doubao"), 10);
        bool found = false;
        for (const SearchResult &r : doubaoHits)
            if (r.record.displayName == QStringLiteral("Doubao.exe"))
                found = true;
        check(found, QStringLiteral("搜 doubao 命中 AppData 下的 Doubao.exe"));

        // userAppDataRoots() 在 Windows 上应返回 Local / Roaming，其余平台返回空
#ifdef Q_OS_WIN
        const QStringList appDataRoots = Scanner::userAppDataRoots();
        bool hasLocal = false;
        for (const QString &r : appDataRoots) {
            const QString tail = QDir::toNativeSeparators(r);
            if (tail.endsWith(QStringLiteral("AppData\\Local"), Qt::CaseInsensitive)
                || tail.endsWith(QStringLiteral("AppData\\Roaming"), Qt::CaseInsensitive))
                hasLocal = true;
        }
        check(!appDataRoots.isEmpty() && hasLocal,
              QStringLiteral("userAppDataRoots() 返回本地 AppData（%1 条）").arg(appDataRoots.size()));
#else
        check(Scanner::userAppDataRoots().isEmpty(),
              QStringLiteral("非 Windows 平台 userAppDataRoots() 返回空"));
#endif
    }

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
    QScreen *screen = listeryScreenAtCursor();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect();
    const int expectedX = available.left() + (available.width() - probe.width()) / 2;
    const int expectedY = available.top() + qMax(48, available.height() * 12 / 100);
    check(qAbs(probe.x() - expectedX) <= 1 && qAbs(probe.y() - expectedY) <= 1,
          QStringLiteral("弹窗水平居中、垂直靠上（x=%1, y=%2）").arg(probe.x()).arg(probe.y()));

    SearchService service(databasePath);
    QString searchError;
    const bool serviceStarted = service.start(&searchError);
    check(serviceStarted, QStringLiteral("后台检索线程启动：%1").arg(searchError));

    LauncherWindow window(&service, 10);
    window.setQueryForPreview(QStringLiteral("zzzzzz"));
    check(window.waitForSearch(3000), QStringLiteral("异步检索在 3 秒内返回结果"));
    const int compactHeight = window.height();
    window.setQueryForPreview(QStringLiteral("alpha"));
    window.waitForSearch(3000);
    const int expandedHeight = window.height();
    window.setQueryForPreview(QString());
    const int inputOnlyHeight = window.height();
    check(compactHeight == 148,
          QStringLiteral("无候选时不展开列表，窗口高度 %1（期望 148）").arg(compactHeight));
    check(expandedHeight == 203,
          QStringLiteral("有候选时展开列表，窗口高度 %1（期望 203）").arg(expandedHeight));
    check(inputOnlyHeight == 112,
          QStringLiteral("空输入只保留输入框，窗口高度 %1（期望 112）").arg(inputOnlyHeight));

    // ---- 窗口几何：真实“显示过”的窗口展开后必须能缩回 ----
    // 顶层窗口 show() 后，布局的 SetDefaultConstraint 会把 minimumHeight 抬到展开高度，
    // 之后 resize() 会被最小高度钳住 —— 这正是“Enter 启动后再唤起面板变高”的原因。
    LauncherWindow shownWindow(&service, 10);
    shownWindow.setAttribute(Qt::WA_DontShowOnScreen, true);
    shownWindow.show();
    shownWindow.setQueryForPreview(QStringLiteral("alpha"));
    shownWindow.waitForSearch(3000);
    const int shownExpanded = shownWindow.height();
    shownWindow.dismiss();
    shownWindow.popup();
    const int shownCompact = shownWindow.height();
    check(shownExpanded == 203,
          QStringLiteral("显示后的窗口展开到 %1（期望 203）").arg(shownExpanded));
    check(shownCompact == 112,
          QStringLiteral("展开后再唤起缩回输入框高度 %1（期望 112）").arg(shownCompact));

    // 连续输入两次：只允许最新一次请求的结果落进列表
    window.setQueryForPreview(QStringLiteral("alpha"));
    window.setQueryForPreview(QStringLiteral("zzzzzz"));
    check(window.waitForSearch(3000) && window.height() == 148,
          QStringLiteral("连续输入时丢弃过期的检索结果"));

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

    // ---- 6.5 插件宿主与组合键 ----
    {
        const HotkeySpec altE = parseHotkey(QStringLiteral("Alt+E"));
        check(altE.valid() && altE.alt && !altE.ctrl && altE.key == Qt::Key_E,
              QStringLiteral("解析组合键 Alt+E：key=%1 alt=%2").arg(altE.key).arg(altE.alt));
        check(normalizeHotkey(QStringLiteral("alt + e")) == QStringLiteral("Alt+E"),
              QStringLiteral("热键写法归一化：%1").arg(normalizeHotkey(QStringLiteral("alt + e"))));
        check(hotkeyMatches(altE, HotkeyModifier::ModAlt, Qt::Key_E)
                  && !hotkeyMatches(altE, HotkeyModifier::ModAlt | HotkeyModifier::ModCtrl, Qt::Key_E)
                  && !hotkeyMatches(altE, HotkeyModifier::ModAlt, Qt::Key_F)
                  && !hotkeyMatches(altE, 0, Qt::Key_E),
              QStringLiteral("组合键命中判定：修饰键严格相等，多按 Ctrl 或多按其它键都不触发"));
        check(!parseHotkey(QStringLiteral("Alt+")).valid()
                  && normalizeHotkey(QStringLiteral("Ctrl+")).isEmpty()
                  && normalizeHotkey(QStringLiteral("Hello")).isEmpty(),
              QStringLiteral("无法识别的热键组合被拒绝"));

        PluginHost host;
        const QString pluginDir = QCoreApplication::applicationDirPath()
                                  + QStringLiteral("/plugins");
        QString loadError;
        const int loaded = host.loadFromDirectory(pluginDir, &loadError);
        const QVector<PluginInfo> plugins = host.plugins();
        bool translatorFound = false;
        bool translatorLoaded = false;
        QString translatorHotkey;
        for (const PluginInfo &info : plugins) {
            if (info.id == QStringLiteral("com.listery.translator")) {
                translatorFound = true;
                translatorLoaded = info.loaded;
                translatorHotkey = info.defaultHotkey;
            }
        }
        check(translatorFound,
              QStringLiteral("plugins 目录里发现翻译插件（已加载 %1 个，目录 %2）")
                  .arg(loaded).arg(pluginDir));
        check(translatorLoaded, QStringLiteral("翻译插件初始化成功"));
        check(translatorHotkey.compare(QStringLiteral("Alt+E"), Qt::CaseInsensitive) == 0,
              QStringLiteral("翻译插件默认热键为 Alt+E（实际 %1）").arg(translatorHotkey));
        check(host.pluginForHotkey(QStringLiteral("Alt+E"))
                  == QStringLiteral("com.listery.translator"),
              QStringLiteral("按热键能反查到插件"));
        host.setEnabled(QStringLiteral("com.listery.translator"), false);
        check(host.pluginForHotkey(QStringLiteral("Alt+E")).isEmpty(),
              QStringLiteral("禁用后热键不再命中该插件"));
        host.setEnabled(QStringLiteral("com.listery.translator"), true);

        // 真实网络查询：拿不到结果不算失败（离线/CI 无外网时只做记录）
        if (translatorLoaded) {
            quint64 sequence = 0;
            QVector<PluginRow> rows;
            QString queryError;
            bool finished = false;
            QObject::connect(&host, &PluginHost::queryFinished,
                             &host, [&](quint64 seq, QString, QString, QVector<PluginRow> result,
                                        QString error) {
                        sequence = seq;
                        rows = result;
                        queryError = error;
                        finished = true;
                    });
            const quint64 issued = host.beginQuery(QStringLiteral("com.listery.translator"),
                                                   QStringLiteral("hello"), 5);
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            QObject::connect(&host, &PluginHost::queryFinished, &loop, &QEventLoop::quit);
            timer.start(8000);
            if (issued == 0)
                finished = true; // 插件没接受请求：直接跳过等待
            else
                loop.exec();

            check(issued != 0, QStringLiteral("插件受理了查询请求"));
            check(finished && sequence == issued,
                  QStringLiteral("插件回调 finish 且序号一致（seq=%1）").arg(sequence));
            if (!rows.isEmpty()) {
                bool hasTitle = true;
                bool hasCopyAction = true;
                for (const PluginRow &row : rows) {
                    if (row.title.isEmpty())
                        hasTitle = false;
                    if (!row.action.startsWith(QStringLiteral("copy:")))
                        hasCopyAction = false;
                }
                check(hasTitle && hasCopyAction,
                      QStringLiteral("翻译结果 %1 条，均带标题与 copy 动作（首条：%2）")
                          .arg(rows.size()).arg(rows.first().title));
            } else {
                qInfo().noquote() << QStringLiteral("[INFO] 翻译插件本次没有返回结果（%1），"
                                                     "离线或被限流时属正常")
                                         .arg(queryError.isEmpty() ? QStringLiteral("无错误信息")
                                                                  : queryError);
            }
        }

        // 面板插件模式：进入后提示语改变，Esc 后回到程序模式
        LauncherWindow window(nullptr, 10);
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        window.setPluginHost(&host);
        window.beginPluginSession(QStringLiteral("com.listery.translator"),
                                  QStringLiteral("英汉翻译"),
                                  QStringLiteral("输入要翻译的内容"));
        check(window.mode() == LauncherMode::Plugin, QStringLiteral("面板进入插件模式"));
        window.endPluginSession();
        check(window.mode() == LauncherMode::Programs, QStringLiteral("退出插件模式回到程序模式"));
    }

    // ---- 7. 全局键盘钩子 ----
#ifdef Q_OS_WIN
    GlobalHotkeys hook;
    hook.setIntervalMs(300);
    hook.start();
    QThread::msleep(500);
    const bool hookRunning = hook.isRunning();
    hook.requestStop();
    hook.wait(2000);
    check(hookRunning, QStringLiteral("WH_KEYBOARD_LL 全局钩子安装成功并保持运行"));
#else
    // 非 Windows 平台：全局钩子分别走 XRecord / CGEventTap，校验“可用性判断”与运行环境一致
    const bool supported = GlobalHotkeys::isSupported();
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
