#include "Core/Scanner.h"

#include "Core/Database.h"
#include "Core/Pinyin.h"
#include "Core/QtCompat.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QStorageInfo>
#include <QStringList>

#include <algorithm>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace {
const int kMaxDepth = 64;

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
} // namespace

Scanner::Scanner(const QString &databasePath,
                 const QStringList &extensions,
                 const QStringList &skipPatterns,
                 bool includeRemovable,
                 qint64 generation,
                 const QStringList &rootsOverride,
                 QObject *parent)
    : QThread(parent)
    , m_databasePath(databasePath)
    , m_extensions(extensions)
    , m_skipPatterns(skipPatterns)
    , m_includeRemovable(includeRemovable)
    , m_generation(generation)
    , m_rootsOverride(rootsOverride)
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

void Scanner::run()
{
    m_progressTimer.start();

    Database db(m_databasePath);
    QString error;
    if (!db.open(&error)) {
        emit scanFinished(0, 0, false, QStringLiteral("打开索引库失败：%1").arg(error));
        return;
    }

    const QStringList roots = m_rootsOverride.isEmpty()
        ? availableRoots(m_includeRemovable)
        : m_rootsOverride;
    if (roots.isEmpty()) {
        emit scanFinished(0, 0, false, QStringLiteral("没有找到可扫描的磁盘卷"));
        return;
    }

    // 标记“扫描进行中”：进程被中途杀掉时，下次启动能识别出索引不完整并补扫
    db.setMeta(QStringLiteral("scan_in_progress"), QStringLiteral("1"));
    db.beginBulk();

    // 开始菜单位于隐藏目录（ProgramData / AppData）下，全盘遍历的 QDir::Dirs 过滤不会进入，
    // 但那里的快捷方式恰恰是最常用的入口，单独扫一遍
    const QStringList menus = m_rootsOverride.isEmpty() ? startMenuRoots() : QStringList();
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
    db.commitBulk();

    const bool canceled = isStopping();
    int removed = 0;
    if (m_error.isEmpty() && !canceled) {
        removed = db.removeStale(m_generation, &error);
        if (removed < 0)
            m_error = QStringLiteral("清理旧索引失败：%1").arg(error);
    }

    // 只有完整跑完且清理成功才算“已更新”；取消或失败时索引是半新半旧的
    if (m_error.isEmpty() && !canceled) {
        db.setMeta(QStringLiteral("last_scan_time"),
                   QString::number(QDateTime::currentMSecsSinceEpoch()));
        db.setMeta(QStringLiteral("last_scan_count"), QString::number(m_filesFound));
        db.setMeta(QStringLiteral("scan_in_progress"), QString());
    }
    db.setMeta(QStringLiteral("last_scan_error"), m_error);
    emit scanFinished(m_filesFound, removed, canceled, m_error);
}

void Scanner::scanDirectory(const QString &path, int depth, Database &db)
{
    if (isStopping() || depth > kMaxDepth)
        return;

    QDir dir(path);
    if (!dir.exists())
        return;

    // 不能加 QDir::NoSymLinks：开始菜单里的 .lnk 带重解析点属性，会被当作符号链接整批滤掉
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::NoSort);

    for (const QFileInfo &info : entries) {
        if (isStopping())
            return;

        if (info.isDir()) {
            // 符号链接 / Windows 目录联接可能形成环，直接跳过
            if (info.isSymLink())
                continue;
            if (isSkippedDirectory(info.fileName()))
                continue;
            ++m_directories;
            scanDirectory(info.absoluteFilePath(), depth + 1, db);
            emitProgress(m_currentRoot);
            continue;
        }

        if (!info.isFile())
            continue;

        const QString suffix = info.suffix().toLower();
        if (!m_extensionSet.contains(suffix))
            continue;

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
            return;
        }

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

bool Scanner::isSkippedDirectory(const QString &name) const
{
    const QString lower = name.toLower();
    for (const QString &pattern : m_skipPatterns) {
        if (QDir::match(pattern.toLower(), lower))
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
