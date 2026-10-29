#include "Core/Scanner.h"

#include "Core/Database.h"
#include "Core/Pinyin.h"
#include "Core/QtCompat.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStringList>

#include <algorithm>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

#if defined(Q_OS_LINUX)
#  include <sys/resource.h>
#endif

namespace {
const int kMaxDepth = 64;
/** locate 单次输出的上限，防止异常环境返回几十万行把内存吃光。 */
const int kLocateLineLimit = 200000;

#ifdef Q_OS_WIN
UINT windowsDriveType(const QString &root)
{
    const QString native = QDir::toNativeSeparators(root);
    return GetDriveTypeW(reinterpret_cast<const wchar_t *>(native.utf16()));
}

/** 所有用户 + 当前用户开始菜单。两者都在隐藏目录下，全盘遍历进不去。 */
QStringList startMenuRoots()
{
    QStringList roots;
    const QString programData = qEnvironmentVariable("ProgramData");
    if (!programData.isEmpty()) {
        roots.append(QDir::toNativeSeparators(
            programData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs")));
    }
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) {
        roots.append(QDir::toNativeSeparators(
            appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs")));
    }
    return roots;
}
#else
QStringList startMenuRoots()
{
    return QStringList();
}
#endif

#ifdef Q_OS_UNIX
QStringList locateDatabasePaths()
{
    return QStringList()
        << QStringLiteral("/var/lib/plocate/plocate.db")
        << QStringLiteral("/var/lib/mlocate/mlocate.db")
        << QStringLiteral("/var/cache/locate/locatedb")
        << QStringLiteral("/var/lib/locate/locatedb")
        << QStringLiteral("/var/db/locate.database");
}
#endif
} // namespace

Scanner::Scanner(const QString &databasePath,
                 const QStringList &extensions,
                 const QStringList &skipPatterns,
                 bool includeRemovable,
                 qint64 generation,
                 const QStringList &volumes,
                 bool deep,
                 QObject *parent)
    : QThread(parent)
    , m_databasePath(databasePath)
    , m_extensions(extensions)
    , m_skipPatterns(skipPatterns)
    , m_includeRemovable(includeRemovable)
    , m_generation(generation)
    , m_volumes(volumes)
    , m_deep(deep)
{
    for (const QString &ext : m_extensions)
        m_extensionSet.insert(ext.toLower());

    // 开始菜单 / 桌面上的快捷方式就是最常用的启动入口，不受扩展名设置影响，始终索引
    m_extensionSet.insert(QStringLiteral("lnk"));
    m_extensionSet.insert(QStringLiteral("url"));
}

Scanner::~Scanner()
{
    requestStop();
    if (isRunning())
        wait(8000);
}

void Scanner::requestStop()
{
    m_stop.storeRelease(1);
}

bool Scanner::isStopping() const
{
    return m_stop.loadAcquire() != 0;
}

QStringList Scanner::availableRoots(bool includeRemovable)
{
    QStringList roots;
    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &volume : volumes) {
        if (!volume.isValid() || !volume.isReady() || volume.rootPath().isEmpty())
            continue;
        if (volume.isReadOnly())
            continue;

#ifdef Q_OS_WIN
        const UINT type = windowsDriveType(volume.rootPath());
        if (type == DRIVE_FIXED)
            roots.append(volume.rootPath());
        else if (includeRemovable && type == DRIVE_REMOVABLE)
            roots.append(volume.rootPath());
        // 网络盘（DRIVE_REMOTE）不参与全盘索引，避免拖慢启动与遍历
#else
        // Unix：只扫本地挂载卷，跳过 /proc、/sys 这类伪文件系统
        const QString rootPath = volume.rootPath();
        static const QStringList pseudoMounts = QStringList()
            << QStringLiteral("/proc") << QStringLiteral("/sys") << QStringLiteral("/dev")
            << QStringLiteral("/run") << QStringLiteral("/tmp");
        bool pseudo = false;
        for (const QString &prefix : pseudoMounts) {
            if (rootPath == prefix || rootPath.startsWith(prefix + QLatin1Char('/'))) {
                pseudo = true;
                break;
            }
        }
        if (pseudo)
            continue;
        // 只收真实块设备挂载的卷（/dev/sdX、/dev/nvme、/dev/mapper 等），排除网络与伪挂载
        if (volume.device().startsWith("/dev/"))
            roots.append(rootPath);
#endif
    }

    roots.removeDuplicates();
    std::sort(roots.begin(), roots.end());
    return roots;
}

QString Scanner::locateProgram()
{
#ifdef Q_OS_UNIX
    const QStringList candidates = QStringList()
        << QStringLiteral("plocate") << QStringLiteral("locate") << QStringLiteral("mlocate");
    for (const QString &candidate : candidates) {
        const QString path = QStandardPaths::findExecutable(candidate);
        if (!path.isEmpty())
            return path;
    }
#endif
    return QString();
}

qint64 Scanner::locateDatabaseMtime()
{
#ifdef Q_OS_UNIX
    qint64 newest = 0;
    for (const QString &path : locateDatabasePaths()) {
        const QFileInfo info(path);
        if (!info.exists())
            continue;
        newest = qMax(newest, info.lastModified().toMSecsSinceEpoch());
    }
    return newest;
#else
    return 0;
#endif
}

bool Scanner::locateAvailable()
{
    return !locateProgram().isEmpty() && locateDatabaseMtime() > 0;
}

QString Scanner::extensionPattern(const QStringList &extensions)
{
    // locate 的 -r 走 POSIX 扩展正则；配合 -i 做大小写不敏感匹配
    QString body = QStringLiteral("\\.(");
    bool first = true;
    for (const QString &ext : extensions) {
        if (ext.isEmpty())
            continue;
        if (!first)
            body.append(QLatin1Char('|'));
        body.append(ext);
        first = false;
    }
    body.append(QStringLiteral(")$"));
    return body;
}

void Scanner::run()
{
    // 后台扫描不该和前台抢资源：Windows 直接降线程优先级，Linux 把 nice 值抬高（只影响本线程）
#ifdef Q_OS_WIN
    setPriority(QThread::IdlePriority);
#elif defined(Q_OS_LINUX)
    (void)setpriority(PRIO_PROCESS, 0, 10);
#endif

    m_progressTimer.start();

    Database db(m_databasePath);
    QString error;
    if (!db.open(&error)) {
        emit scanFinished(0, 0, false, QStringLiteral("打开索引库失败：%1").arg(error));
        return;
    }

    const QStringList roots = m_volumes.isEmpty()
        ? availableRoots(m_includeRemovable)
        : m_volumes;
    if (roots.isEmpty()) {
        emit scanFinished(0, 0, false, QStringLiteral("没有找到可扫描的磁盘卷"));
        return;
    }

    // 标记“扫描进行中”：进程被中途杀掉时，下次启动能识别出索引不完整并补扫
    db.setMeta(QStringLiteral("scan_in_progress"), QStringLiteral("1"));
    db.beginBulk();

    // Linux 上优先读 locate 快照：几十万条目录的遍历要几分钟，locate 只要一两秒
    m_usedLocate = !m_deep && scanViaLocate(roots, db);

    if (!m_usedLocate) {
        // 开始菜单位于隐藏目录（ProgramData / AppData）下，全盘遍历的 QDir::Dirs 过滤不会进入，
        // 但那里的快捷方式恰恰是最常用的入口，单独扫一遍
        const QStringList menus = m_volumes.isEmpty() ? startMenuRoots() : QStringList();
        for (const QString &menu : menus) {
            if (isStopping())
                break;
            m_currentRoot = menu;
            emitProgress(menu, true);
            scanDirectory(menu, 0, db);
        }

        for (const QString &root : roots) {
            if (isStopping())
                break;
            m_currentRoot = root;
            emitProgress(root, true);
            scanDirectory(root, 0, db);

            if (m_error.isEmpty() && !isStopping()) {
                db.commitBulk();
                db.beginBulk();
                m_pendingWrites = 0;
            }
        }
    }
    db.commitBulk();

    const bool canceled = isStopping();
    int removed = 0;
    if (m_error.isEmpty() && !canceled) {
        // 只清理这次扫过的卷；走 locate 快照时保留比快照更新的记录（它们可能还没进 locate 库）
        removed = db.removeStale(m_generation, roots, m_usedLocate ? m_locateDbMtime : 0, &error);
        if (removed < 0)
            m_error = QStringLiteral("清理旧索引失败：%1").arg(error);
    }

    // 只有完整跑完且清理成功才算“已更新”；取消或失败时索引是半新半旧的
    if (m_error.isEmpty() && !canceled) {
        for (const QString &root : roots)
            recordVolumeStat(root, db);

        db.setMeta(QStringLiteral("last_scan_time"),
                   QString::number(QDateTime::currentMSecsSinceEpoch()));
        db.setMeta(QStringLiteral("last_scan_count"), QString::number(m_filesFound));
        db.setMeta(QStringLiteral("scan_in_progress"), QString());
    }
    db.setMeta(QStringLiteral("last_scan_error"), m_error);
    emit scanFinished(m_filesFound, removed, canceled, m_error);
}

void Scanner::recordVolumeStat(const QString &root, Database &db)
{
    VolumeStat stat = db.volumeStat(root);
    const int previousCount = stat.exeCount;
    const bool previouslyScanned = stat.lastScan > 0;
    const int count = db.countInVolume(root);

    stat.lastScan = QDateTime::currentMSecsSinceEpoch();
    stat.exeCount = count;

    if (count > 0) {
        stat.emptyRounds = 0;
    } else if (m_usedLocate && previouslyScanned && previousCount > 0) {
        // 之前扫到过、这一轮 locate 却一条都没有：多半是 locate 库太旧，下一轮全量复核
        stat.emptyRounds = 0;
        stat.deepPending = true;
    } else {
        stat.emptyRounds = qMin(stat.emptyRounds + 1, 8);
    }

    if (m_deep)
        stat.deepPending = false;

    db.setVolumeStat(stat);
}

bool Scanner::scanViaLocate(const QStringList &roots, Database &db)
{
#ifndef Q_OS_UNIX
    Q_UNUSED(roots)
    Q_UNUSED(db)
    return false;
#else
    const QString program = locateProgram();
    if (program.isEmpty())
        return false;

    const qint64 dbMtime = locateDatabaseMtime();
    if (dbMtime <= 0)
        return false;

    // 两类候选：白名单后缀 + Unix 上常见的无扩展名可执行文件（/usr/bin、~/.local/bin …）
    const QStringList patterns = QStringList()
        << extensionPattern(m_extensions)
        << QStringLiteral("/(bin|sbin|games)/[^/]+$");

    QStringList candidates;
    for (const QString &pattern : patterns) {
        QProcess process;
        process.start(program, QStringList() << QStringLiteral("-i") << QStringLiteral("-r") << pattern);
        if (!process.waitForStarted(5000))
            return false;
        if (!process.waitForFinished(180000)) {
            process.kill();
            process.waitForFinished(5000);
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit)
            return false;

        const QList<QByteArray> lines = process.readAllStandardOutput().split('\n');
        for (const QByteArray &line : lines) {
            if (candidates.size() >= kLocateLineLimit)
                break;
            const QString path = QString::fromLocal8Bit(line).trimmed();
            if (!path.isEmpty())
                candidates.append(path);
        }
    }

    candidates.removeDuplicates();
    m_locateDbMtime = dbMtime;

    for (const QString &path : candidates) {
        if (isStopping())
            return true;

        bool underRoot = false;
        for (const QString &root : roots) {
            if (path.startsWith(root)) {
                underRoot = true;
                break;
            }
        }
        if (!underRoot)
            continue;
        if (isSkippedPath(path))
            continue;

        const QFileInfo info(path);
        // 无扩展名的候选必须真的带可执行位（bin 目录里也常有数据文件与脚本片段）
        if (!ingestFile(info, db, true))
            continue;

        ++m_filesFound;
        ++m_pendingWrites;
        if (m_pendingWrites >= 2000) {
            db.commitBulk();
            db.beginBulk();
            m_pendingWrites = 0;
        }
        emitProgress(path);
    }
    return true;
#endif
}

void Scanner::scanDirectory(const QString &path, int depth, Database &db)
{
    if (isStopping() || depth > kMaxDepth)
        return;

    QDir dir(path);
    if (!dir.exists())
        return;

    // 不能加 QDir::NoSymLinks：开始菜单里的 .lnk 带重解析点属性，会被当作符号链接整批滤掉。
    // QDirIterator 逐个产出，比 entryInfoList 少一次整目录的 QFileInfo 数组分配；
    // 目录项的类型在 Windows 来自 FindFirstFile、在 Unix 来自 readdir 的 d_type，不需要额外 stat。
    QDirIterator it(path, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        if (isStopping())
            return;

        it.next();
        const QString name = it.fileName();
        const QFileInfo info = it.fileInfo();

        if (info.isDir()) {
            // 符号链接 / Windows 目录联接可能形成环，直接跳过
            if (info.isSymLink())
                continue;
            if (isSkippedDirectory(name))
                continue;
            ++m_directories;
            scanDirectory(info.absoluteFilePath(), depth + 1, db);
            emitProgress(m_currentRoot);
            continue;
        }

        // 先用名字把绝大多数无关文件挡掉，再去做需要 stat 的判断：
        // 一个典型的系统盘里 95% 以上的文件都不是可执行文件，这一步省掉了它们的系统调用
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot <= 0 || dot == name.size() - 1)
            continue;
        if (!m_extensionSet.contains(name.mid(dot + 1).toLower()))
            continue;

        if (!ingestFile(info, db, false))
            continue;

        ++m_filesFound;
        ++m_pendingWrites;
        if (m_pendingWrites >= 2000) {
            db.commitBulk();
            db.beginBulk();
            m_pendingWrites = 0;
        }
        emitProgress(m_currentRoot);
    }
}

bool Scanner::ingestFile(const QFileInfo &info, Database &db, bool allowExtensionless)
{
    if (!info.isFile())
        return false;

    const QString suffix = info.suffix().toLower();
    const bool matched = m_extensionSet.contains(suffix);
    if (!matched && !(allowExtensionless && info.isExecutable()))
        return false;

    ExecutableRecord record;
    record.path = QDir::toNativeSeparators(info.absoluteFilePath());
    // 快捷方式显示原名（"腾讯会议"），带上 .lnk 只是噪音
    record.displayName = (suffix == QLatin1String("lnk") || suffix == QLatin1String("url"))
        ? info.completeBaseName()
        : info.fileName();
    record.dir = QDir::toNativeSeparators(info.absolutePath());
    record.ext = suffix;
    record.size = info.size();
    record.mtime = info.lastModified().toMSecsSinceEpoch();
    Pinyin::buildSearchForms(record.displayName, &record.pinyin, &record.pinyinInitials);

    if (!db.upsertExecutable(record, m_generation)) {
        m_error = QStringLiteral("写入索引失败：%1").arg(record.path);
        m_stop.storeRelease(1);
        return false;
    }
    return true;
}

bool Scanner::isSkippedDirectory(const QString &name) const
{
    const QString lower = name.toLower();
    for (const QString &pattern : m_skipPatterns) {
        if (QDir::match(pattern.toLower(), lower))
            return true;
    }
    return false;
}

bool Scanner::isSkippedPath(const QString &path) const
{
    if (m_skipPatterns.isEmpty())
        return false;

    const QStringList parts = path.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (isSkippedDirectory(part))
            return true;
    }
    return false;
}

void Scanner::emitProgress(const QString &root, bool force)
{
    if (!force && m_progressTimer.elapsed() < 400)
        return;
    m_progressTimer.restart();
    emit progressChanged(root, m_directories, m_filesFound);
}
