#include "Core/AppSettings.h"

#include "Core/Localization.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QtGlobal>

namespace {
const char *kKeyExtensions = "index/extensions";
const char *kKeySkipDirs = "index/skipDirectories";
const char *kKeyIncludeRemovable = "index/includeRemovableDrives";
const char *kKeyAutoStart = "general/autoStart";
const char *kKeyHotkeyInterval = "ui/hotkeyIntervalMs";
const char *kKeyMaxResults = "ui/maxResults";
const char *kKeyLanguage = "ui/language";
const char *kKeyAutoScan = "index/autoScanOnStartup";
const char *kKeyRescanMinutes = "index/rescanIntervalMinutes";
const char *kKeyHideOnFocusLost = "ui/hideWhenFocusLost";
const char *kKeySmartSchedule = "index/smartSchedule";
const char *kKeyUseLocate = "index/useLocateIndex";
const char *kKeyAutoUpdate = "update/autoCheck";
const char *kKeyLastUpdateCheck = "update/lastCheck";
const char *kKeySchemaVersion = "meta/schemaVersion";
/** 跳过目录默认值的版本：升到 2 时把 AppData 下的缓存目录补进老用户的配置。 */
const char *kKeySkipDirsVersion = "index/skipDirsVersion";
const int kVSkipDirsVersion = 2;

QStringList defaultExtensions()
{
    return QStringList() << QStringLiteral("exe") << QStringLiteral("com")
                         << QStringLiteral("bat") << QStringLiteral("cmd")
                         << QStringLiteral("sh") << QStringLiteral("ps1");
}

QStringList defaultSkipPatterns()
{
    return QStringList() << QStringLiteral("WinSxS")
                         << QStringLiteral("$Recycle.Bin")
                         << QStringLiteral("System Volume Information")
                         << QStringLiteral("node_modules")
                         << QStringLiteral(".git")
                         << QStringLiteral("SoftwareDistribution")
                         << QStringLiteral("DriverStore")
                         << QStringLiteral("WindowsApps")
                         // UWP 的本地数据，几十万个碎片文件且从不放可启动程序
                         << QStringLiteral("Packages")
                         // 下面这些是 AppData 里的浏览器/编辑器缓存，扫它们纯属浪费 IO
                         << QStringLiteral("Cache")
                         << QStringLiteral("Code Cache")
                         << QStringLiteral("GPUCache")
                         << QStringLiteral("CachedData")
                         << QStringLiteral("Service Worker")
                         << QStringLiteral("Crashpad")
                         << QStringLiteral("blob_storage")
                         << QStringLiteral("Temp")
                         << QStringLiteral("__pycache__");
}

/** 实际写一个探针文件，避免把只读目录误判为可写。 */
bool isDirectoryWritable(const QString &dir)
{
    QDir directory(dir);
    if (!directory.exists())
        return false;

    QFile probe(directory.filePath(QStringLiteral(".listery-write-test")));
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    probe.close();
    probe.remove();
    return true;
}
} // namespace

QString AppSettings::resolveDataDir()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (!exeDir.isEmpty() && isDirectoryWritable(exeDir))
        return exeDir;

    QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (fallback.isEmpty())
        fallback = QDir::homePath() + QStringLiteral("/.listery");
    return fallback;
}

AppSettings::AppSettings()
{
    m_dataDir = resolveDataDir();
    ensureDirectory(m_dataDir);

    m_settings = new QSettings(settingsFilePath(), QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    m_settings->setIniCodec("UTF-8");
#endif
    ensureDefaults();
}

AppSettings::~AppSettings()
{
    delete m_settings;
}

void AppSettings::reload()
{
    m_settings->sync();
}

QString AppSettings::dataDir() const
{
    return m_dataDir;
}

QString AppSettings::databasePath() const
{
    return m_dataDir + QStringLiteral("/index.db");
}

QString AppSettings::logFilePath() const
{
    return m_dataDir + QStringLiteral("/listery.log");
}

QString AppSettings::settingsFilePath() const
{
    return m_dataDir + QStringLiteral("/settings.ini");
}

QStringList AppSettings::executableExtensions() const
{
    QStringList exts = m_settings->value(QString::fromLatin1(kKeyExtensions),
                                         defaultExtensions()).toStringList();
    QStringList cleaned;
    for (const QString &ext : exts) {
        QString value = ext.trimmed().toLower();
        if (value.startsWith(QLatin1Char('.')))
            value.remove(0, 1);
        if (!value.isEmpty() && !cleaned.contains(value))
            cleaned.append(value);
    }
    return cleaned.isEmpty() ? defaultExtensions() : cleaned;
}

void AppSettings::setExecutableExtensions(const QStringList &exts)
{
    m_settings->setValue(QString::fromLatin1(kKeyExtensions), exts);
    m_settings->sync();
}

QStringList AppSettings::skipDirectoryPatterns() const
{
    return m_settings->value(QString::fromLatin1(kKeySkipDirs),
                             defaultSkipPatterns()).toStringList();
}

void AppSettings::setSkipDirectoryPatterns(const QStringList &patterns)
{
    m_settings->setValue(QString::fromLatin1(kKeySkipDirs), patterns);
    m_settings->sync();
}

bool AppSettings::includeRemovableDrives() const
{
    // 默认不索引可移动磁盘：U 盘/移动硬盘通常没有要启动的程序，
    // 而且插拔会触发不必要的全盘遍历，拖慢启动与扫描。
    return m_settings->value(QString::fromLatin1(kKeyIncludeRemovable), false).toBool();
}

void AppSettings::setIncludeRemovableDrives(bool on)
{
    m_settings->setValue(QString::fromLatin1(kKeyIncludeRemovable), on);
    m_settings->sync();
}

bool AppSettings::autoStart() const
{
    return m_settings->value(QString::fromLatin1(kKeyAutoStart), true).toBool();
}

void AppSettings::setAutoStart(bool on)
{
    m_settings->setValue(QString::fromLatin1(kKeyAutoStart), on);
    m_settings->sync();
}

int AppSettings::hotkeyIntervalMs() const
{
    return qBound(150, m_settings->value(QString::fromLatin1(kKeyHotkeyInterval), 300).toInt(), 800);
}

void AppSettings::setHotkeyIntervalMs(int ms)
{
    m_settings->setValue(QString::fromLatin1(kKeyHotkeyInterval), qBound(150, ms, 800));
    m_settings->sync();
}

int AppSettings::maxResults() const
{
    return qBound(5, m_settings->value(QString::fromLatin1(kKeyMaxResults), 12).toInt(), 30);
}

void AppSettings::setMaxResults(int count)
{
    m_settings->setValue(QString::fromLatin1(kKeyMaxResults), qBound(5, count, 30));
    m_settings->sync();
}

QString AppSettings::language() const
{
    // 没设置过时不写入默认值，这样用户换系统语言后界面也会跟着变
    const QString stored =
        m_settings->value(QString::fromLatin1(kKeyLanguage), QString()).toString();
    return stored.isEmpty() ? Localization::systemLanguage() : stored;
}

void AppSettings::setLanguage(const QString &code)
{
    if (code.isEmpty())
        m_settings->remove(QString::fromLatin1(kKeyLanguage));
    else
        m_settings->setValue(QString::fromLatin1(kKeyLanguage), code);
    m_settings->sync();
}

bool AppSettings::autoScanOnStart() const
{
    return m_settings->value(QString::fromLatin1(kKeyAutoScan), true).toBool();
}

void AppSettings::setAutoScanOnStart(bool on)
{
    m_settings->setValue(QString::fromLatin1(kKeyAutoScan), on);
    m_settings->sync();
}

int AppSettings::rescanIntervalMinutes() const
{
    return qBound(0, m_settings->value(QString::fromLatin1(kKeyRescanMinutes), 720).toInt(), 10080);
}

void AppSettings::setRescanIntervalMinutes(int minutes)
{
    m_settings->setValue(QString::fromLatin1(kKeyRescanMinutes), qBound(0, minutes, 10080));
    m_settings->sync();
}

bool AppSettings::hideWhenFocusLost() const
{
    return m_settings->value(QString::fromLatin1(kKeyHideOnFocusLost), true).toBool();
}

void AppSettings::setHideWhenFocusLost(bool on)
{
    m_settings->setValue(QString::fromLatin1(kKeyHideOnFocusLost), on);
    m_settings->sync();
}

bool AppSettings::smartSchedule() const
{
    return m_settings->value(QString::fromLatin1(kKeySmartSchedule), true).toBool();
}

void AppSettings::setSmartSchedule(bool on)
{
    m_settings->setValue(QString::fromLatin1(kKeySmartSchedule), on);
    m_settings->sync();
}

bool AppSettings::useLocateIndex() const
{
    return m_settings->value(QString::fromLatin1(kKeyUseLocate), true).toBool();
}

void AppSettings::setUseLocateIndex(bool on)
{
    m_settings->setValue(QString::fromLatin1(kKeyUseLocate), on);
    m_settings->sync();
}

bool AppSettings::autoCheckUpdates() const
{
    return m_settings->value(QString::fromLatin1(kKeyAutoUpdate), true).toBool();
}

void AppSettings::setAutoCheckUpdates(bool on)
{
    m_settings->setValue(QString::fromLatin1(kKeyAutoUpdate), on);
    m_settings->sync();
}

qint64 AppSettings::lastUpdateCheck() const
{
    return m_settings->value(QString::fromLatin1(kKeyLastUpdateCheck), 0).toLongLong();
}

void AppSettings::setLastUpdateCheck(qint64 timestampMs)
{
    m_settings->setValue(QString::fromLatin1(kKeyLastUpdateCheck), timestampMs);
    m_settings->sync();
}

QString AppSettings::resolvePluginDir()
{
    // 插件始终从可执行文件旁边加载：数据目录不可写时会回退到 AppData，
    // 但插件是随程序分发的，不该跟着走
    return QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
}

QString AppSettings::pluginDir() const
{
    return resolvePluginDir();
}

bool AppSettings::pluginEnabled(const QString &pluginId) const
{
    if (pluginId.isEmpty())
        return false;
    return m_settings->value(QStringLiteral("plugins/%1/enabled").arg(pluginId), true).toBool();
}

void AppSettings::setPluginEnabled(const QString &pluginId, bool enabled)
{
    if (pluginId.isEmpty())
        return;
    m_settings->setValue(QStringLiteral("plugins/%1/enabled").arg(pluginId), enabled);
    m_settings->sync();
}

QString AppSettings::pluginHotkey(const QString &pluginId) const
{
    if (pluginId.isEmpty())
        return QString();
    return m_settings->value(QStringLiteral("plugins/%1/hotkey").arg(pluginId), QString())
        .toString();
}

void AppSettings::setPluginHotkey(const QString &pluginId, const QString &hotkey)
{
    if (pluginId.isEmpty())
        return;
    m_settings->setValue(QStringLiteral("plugins/%1/hotkey").arg(pluginId), hotkey);
    m_settings->sync();
}

int AppSettings::schemaVersion() const
{
    return m_settings->value(QString::fromLatin1(kKeySchemaVersion), 1).toInt();
}

void AppSettings::setSchemaVersion(int version)
{
    m_settings->setValue(QString::fromLatin1(kKeySchemaVersion), version);
    m_settings->sync();
}

void AppSettings::ensureDefaults()
{
    if (!m_settings->contains(QString::fromLatin1(kKeyExtensions)))
        m_settings->setValue(QString::fromLatin1(kKeyExtensions), defaultExtensions());
    if (!m_settings->contains(QString::fromLatin1(kKeySkipDirs)))
        m_settings->setValue(QString::fromLatin1(kKeySkipDirs), defaultSkipPatterns());
    if (!m_settings->contains(QString::fromLatin1(kKeySchemaVersion)))
        m_settings->setValue(QString::fromLatin1(kKeySchemaVersion), 1);

    // 跳过目录的默认值会随版本增补。老用户的配置项已经存在，不能整份覆盖（会吞掉用户改过的值），
    // 只把这一版新增的条目并进去，并记下版本号避免每次启动重复处理。
    const int skipVersion = m_settings->value(QString::fromLatin1(kKeySkipDirsVersion), 1).toInt();
    if (skipVersion < kVSkipDirsVersion) {
        QStringList merged = skipDirectoryPatterns();
        static const QStringList kAddedInV2 = QStringList()
            << QStringLiteral("Packages")
            << QStringLiteral("Cache")
            << QStringLiteral("Code Cache")
            << QStringLiteral("GPUCache")
            << QStringLiteral("CachedData")
            << QStringLiteral("Service Worker")
            << QStringLiteral("Crashpad")
            << QStringLiteral("blob_storage");
        for (const QString &pattern : kAddedInV2) {
            if (!merged.contains(pattern, Qt::CaseInsensitive))
                merged.append(pattern);
        }
        m_settings->setValue(QString::fromLatin1(kKeySkipDirs), merged);
        m_settings->setValue(QString::fromLatin1(kKeySkipDirsVersion), kVSkipDirsVersion);
    }

    // 旧版本遗留的“扫描根目录”配置已废弃，统一改为全盘扫描
    if (m_settings->contains(QStringLiteral("index/roots")))
        m_settings->remove(QStringLiteral("index/roots"));

    m_settings->sync();
}

void AppSettings::ensureDirectory(const QString &path) const
{
    QDir dir;
    if (!dir.exists(path))
        dir.mkpath(path);
}
