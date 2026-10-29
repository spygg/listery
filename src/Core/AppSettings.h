#pragma once

#include <QString>
#include <QStringList>

class QSettings;

/**
 * 应用配置：数据目录、索引规则与界面参数。
 * 配置、索引库与日志统一放在可执行文件所在目录；该目录不可写时回退到用户 AppData。
 */
class AppSettings
{
public:
    AppSettings();
    ~AppSettings();

    void reload();

    /** 可执行文件目录（不可写时回退 AppData），日志与主程序入口也用它。 */
    static QString resolveDataDir();

    QString dataDir() const;
    QString databasePath() const;
    QString logFilePath() const;
    QString settingsFilePath() const;

    // 索引
    QStringList executableExtensions() const;
    void setExecutableExtensions(const QStringList &exts);

    QStringList skipDirectoryPatterns() const;
    void setSkipDirectoryPatterns(const QStringList &patterns);

    bool includeRemovableDrives() const;
    void setIncludeRemovableDrives(bool on);

    // 启动
    bool autoStart() const;
    void setAutoStart(bool on);

    // 交互
    int hotkeyIntervalMs() const;
    void setHotkeyIntervalMs(int ms);

    int maxResults() const;
    void setMaxResults(int count);

    bool autoScanOnStart() const;
    void setAutoScanOnStart(bool on);

    int rescanIntervalMinutes() const;
    void setRescanIntervalMinutes(int minutes);

    bool hideWhenFocusLost() const;
    void setHideWhenFocusLost(bool on);

    // 版本管理
    int schemaVersion() const;
    void setSchemaVersion(int version);

private:
    void ensureDefaults();
    void ensureDirectory(const QString &path) const;

    QSettings *m_settings = nullptr;
    QString m_dataDir;
};
