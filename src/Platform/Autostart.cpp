#include "Platform/Autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtGlobal>

#include <string>

#ifdef Q_OS_WIN
#  include <windows.h>
#elif defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
#  include <QTextStream>
#endif

namespace Autostart {

namespace {
#ifdef Q_OS_WIN
const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t *kValueName = L"Listery";

QString registryErrorText(LONG code)
{
    return QStringLiteral("注册表操作失败，错误码 %1").arg(static_cast<qlonglong>(code));
}
#else
QString autostartFilePath()
{
#ifdef Q_OS_MACOS
    return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/com.listery.launcher.plist");
#else
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return configDir + QStringLiteral("/autostart/listery.desktop");
#endif
}

QString xmlEscaped(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    escaped.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    escaped.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return escaped;
}

bool writeTextFile(const QString &path, const QString &content, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("无法写入 %1：%2").arg(path, file.errorString());
        return false;
    }

    QTextStream stream(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    stream.setCodec("UTF-8");
#endif
    stream << content;
    file.close();
    return true;
}
#endif
} // namespace

bool isSupported()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

QString commandLine()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --autostart").arg(exe);
}

bool isEnabled()
{
#ifdef Q_OS_WIN
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD size = 0;
    const LONG result = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && size > 0;
#else
    return QFileInfo::exists(autostartFilePath());
#endif
}

bool setEnabled(bool on, QString *error)
{
#ifdef Q_OS_WIN
    HKEY key = nullptr;
    const LONG openResult = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr,
                                            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                            &key, nullptr);
    if (openResult != ERROR_SUCCESS) {
        if (error)
            *error = registryErrorText(openResult);
        return false;
    }

    bool ok = false;
    if (on) {
        const QString command = commandLine();
        const std::wstring wide = command.toStdWString();
        const LONG setResult = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                              reinterpret_cast<const BYTE *>(wide.c_str()),
                                              static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
        if (setResult == ERROR_SUCCESS) {
            ok = true;
        } else if (error) {
            *error = registryErrorText(setResult);
        }
    } else {
        const LONG deleteResult = RegDeleteValueW(key, kValueName);
        ok = (deleteResult == ERROR_SUCCESS || deleteResult == ERROR_FILE_NOT_FOUND);
        if (!ok && error)
            *error = registryErrorText(deleteResult);
    }

    RegCloseKey(key);
    return ok;
#else
    const QString path = autostartFilePath();
    if (!on) {
        if (!QFileInfo::exists(path))
            return true;
        if (QFile::remove(path))
            return true;
        if (error)
            *error = QStringLiteral("无法删除自启动文件：%1").arg(path);
        return false;
    }

    const QString exe = QCoreApplication::applicationFilePath();
#ifdef Q_OS_MACOS
    const QString content = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>Label</key>\n"
        "    <string>com.listery.launcher</string>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n"
        "        <string>%1</string>\n"
        "        <string>--autostart</string>\n"
        "    </array>\n"
        "    <key>RunAtLoad</key>\n"
        "    <true/>\n"
        "    <key>ProcessType</key>\n"
        "    <string>Interactive</string>\n"
        "</dict>\n"
        "</plist>\n")
        .arg(xmlEscaped(exe));
#else
    const QString content = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Listery\n"
        "Comment=双击 Ctrl 快速启动器\n"
        "Exec=\"%1\" --autostart\n"
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n")
        .arg(exe);
#endif
    return writeTextFile(path, content, error);
#endif
}

} // namespace Autostart
