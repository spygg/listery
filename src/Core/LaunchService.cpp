#include "Core/LaunchService.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <shellapi.h>

namespace {
const wchar_t *wide(const QString &text)
{
    return reinterpret_cast<const wchar_t *>(text.utf16());
}

QString shellErrorText(int code)
{
    switch (code) {
    case 0:
        return QCoreApplication::translate("LaunchService", "系统资源不足，无法启动该程序");
    case ERROR_FILE_NOT_FOUND:
        return QCoreApplication::translate("LaunchService", "文件不存在，可能已被移动或删除");
    case ERROR_PATH_NOT_FOUND:
        return QCoreApplication::translate("LaunchService", "路径不存在");
    case ERROR_ACCESS_DENIED:
        return QCoreApplication::translate("LaunchService", "没有权限启动该程序（可尝试以管理员身份运行）");
    case ERROR_BAD_FORMAT:
        return QCoreApplication::translate("LaunchService", "不是有效的可执行文件");
    case SE_ERR_ASSOCINCOMPLETE:
    case SE_ERR_NOASSOC:
        return QCoreApplication::translate("LaunchService", "系统没有为该类型文件关联打开方式");
    default:
        return QCoreApplication::translate("LaunchService", "启动失败，ShellExecute 返回码 %1").arg(code);
    }
}
} // namespace
#endif

namespace LaunchService {

QString platformHint()
{
#if defined(Q_OS_WIN)
    return QCoreApplication::translate("LaunchService", "双击 Ctrl 唤起 · Enter 运行 · Ctrl+Enter 定位 · Shift+Enter 管理员");
#elif defined(Q_OS_MACOS)
    return QCoreApplication::translate("LaunchService", "双击 Ctrl 唤起 · Enter 运行 · Ctrl+Enter 定位 · Shift+Enter 管理员");
#else
    return QCoreApplication::translate("LaunchService", "双击 Ctrl 唤起（X11） · Enter 运行 · Ctrl+Enter 定位 · Shift+Enter 管理员");
#endif
}

bool launch(const ExecutableRecord &record, bool asAdmin, QString *errorMessage)
{
    if (record.path.isEmpty()) {
        if (errorMessage)
            *errorMessage = QCoreApplication::translate("LaunchService", "路径为空");
        return false;
    }

    const QFileInfo info(record.path);
    if (!info.exists()) {
        if (errorMessage)
            *errorMessage = QCoreApplication::translate("LaunchService", "文件不存在：%1").arg(record.path);
        return false;
    }

    const QString workDir = info.absolutePath();
    const QString nativePath = QDir::toNativeSeparators(record.path);
    const QString ext = info.suffix().toLower();

#ifdef Q_OS_WIN
    // .sh 在 Windows 上需要 Git Bash / WSL 的 bash
    if (ext == QLatin1String("sh")) {
        const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
        if (bash.isEmpty()) {
            if (errorMessage)
                *errorMessage = QCoreApplication::translate("LaunchService", "启动 .sh 需要 bash：请安装 Git for Windows 或 WSL");
            return false;
        }
        if (!QProcess::startDetached(bash, QStringList() << nativePath, workDir)) {
            if (errorMessage)
                *errorMessage = QCoreApplication::translate("LaunchService", "无法通过 bash 启动脚本");
            return false;
        }
        return true;
    }

    // .ps1 交给 PowerShell 执行，避免被记事本打开
    if (ext == QLatin1String("ps1")) {
        QString shell = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
        if (shell.isEmpty())
            shell = QStandardPaths::findExecutable(QStringLiteral("powershell"));
        if (shell.isEmpty()) {
            if (errorMessage)
                *errorMessage = QCoreApplication::translate("LaunchService", "未找到 PowerShell，无法运行脚本");
            return false;
        }
        const QString params = QStringLiteral("-NoLogo -NoExit -ExecutionPolicy Bypass -File \"%1\"")
                                   .arg(nativePath);
        const HINSTANCE result = ShellExecuteW(nullptr,
                                               asAdmin ? L"runas" : L"open",
                                               wide(shell),
                                               wide(params),
                                               wide(workDir),
                                               SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            if (errorMessage)
                *errorMessage = shellErrorText(static_cast<int>(reinterpret_cast<INT_PTR>(result)));
            return false;
        }
        return true;
    }

    const HINSTANCE result = ShellExecuteW(nullptr,
                                           asAdmin ? L"runas" : L"open",
                                           wide(nativePath),
                                           nullptr,
                                           wide(workDir),
                                           SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        if (errorMessage)
            *errorMessage = shellErrorText(static_cast<int>(reinterpret_cast<INT_PTR>(result)));
        return false;
    }
    return true;
#else
    QString launchTarget = nativePath;

#ifdef Q_OS_MACOS
    // 命中 .app 包内的可执行文件时，优先让 LaunchServices 打开整个 bundle
    const int bundleIndex = record.path.indexOf(QStringLiteral(".app/Contents/MacOS/"));
    if (bundleIndex >= 0)
        launchTarget = record.path.left(bundleIndex + 4);
#endif

    if (asAdmin) {
#ifdef Q_OS_MACOS
        // 提权执行的是 bundle 内的真实二进制，而不是 .app 目录
        QString escaped = nativePath;
        // 索引项就是 .app 包本身（目录）时，直接执行目录会失败：改用它包内的主程序
        if (launchTarget.endsWith(QLatin1String(".app"), Qt::CaseInsensitive)) {
            const QString binary = launchTarget + QStringLiteral("/Contents/MacOS/")
                + QFileInfo(launchTarget).completeBaseName();
            if (QFileInfo(binary).isFile())
                escaped = binary;
        }
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        const QString script =
            QStringLiteral("do shell script (quoted form of \"%1\") with administrator privileges")
                .arg(escaped);
        if (!QProcess::startDetached(QStringLiteral("osascript"),
                                     QStringList() << QStringLiteral("-e") << script)) {
            if (errorMessage)
                *errorMessage = QCoreApplication::translate("LaunchService", "无法调用 osascript 提权启动");
            return false;
        }
        return true;
#else
        const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        if (pkexec.isEmpty()) {
            if (errorMessage)
                *errorMessage = QCoreApplication::translate("LaunchService", "以管理员身份运行需要 polkit 的 pkexec，请先安装 polkit");
            return false;
        }
        if (!QProcess::startDetached(pkexec, QStringList() << launchTarget, workDir)) {
            if (errorMessage)
                *errorMessage = QCoreApplication::translate("LaunchService", "pkexec 启动失败（可能没有图形化 polkit 代理）");
            return false;
        }
        return true;
#endif
    }

#ifdef Q_OS_MACOS
    if (QFileInfo(launchTarget).isDir()) {
        if (QProcess::startDetached(QStringLiteral("open"), QStringList() << launchTarget)) {
            return true;
        }
        if (errorMessage)
            *errorMessage = QCoreApplication::translate("LaunchService", "无法通过 LaunchServices 打开应用包");
        return false;
    }
#endif

    if (ext == QLatin1String("sh") && !info.isExecutable()) {
        const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
        if (bash.isEmpty()) {
            if (errorMessage)
                *errorMessage = QCoreApplication::translate("LaunchService", "未找到 bash，无法运行脚本");
            return false;
        }
        if (!QProcess::startDetached(bash, QStringList() << nativePath, workDir)) {
            if (errorMessage)
                *errorMessage = QCoreApplication::translate("LaunchService", "无法通过 bash 启动脚本");
            return false;
        }
        return true;
    }

    if (!QProcess::startDetached(launchTarget, QStringList(), workDir)) {
        if (errorMessage)
            *errorMessage = QCoreApplication::translate("LaunchService", "启动失败：%1（请确认文件有可执行权限）").arg(launchTarget);
        return false;
    }
    return true;
#endif
}

bool revealInFolder(const QString &filePath, QString *errorMessage)
{
    if (filePath.isEmpty()) {
        if (errorMessage)
            *errorMessage = QCoreApplication::translate("LaunchService", "路径为空");
        return false;
    }

    const QFileInfo info(filePath);
    const QString nativePath = QDir::toNativeSeparators(info.absoluteFilePath());

#ifdef Q_OS_WIN
    const QString params = QStringLiteral("/select,\"%1\"").arg(nativePath);
    const HINSTANCE result = ShellExecuteW(nullptr,
                                           L"open",
                                           L"explorer.exe",
                                           wide(params),
                                           nullptr,
                                           SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        if (errorMessage)
            *errorMessage = shellErrorText(static_cast<int>(reinterpret_cast<INT_PTR>(result)));
        return false;
    }
    return true;
#elif defined(Q_OS_MACOS)
    if (QProcess::startDetached(QStringLiteral("open"),
                                QStringList() << QStringLiteral("-R") << nativePath)) {
        return true;
    }
    if (errorMessage)
        *errorMessage = QCoreApplication::translate("LaunchService", "无法调用 Finder 定位文件");
    return false;
#else
    const QString dir = info.absolutePath();
    const QStringList managers = QStringList() << QStringLiteral("nautilus")
                                               << QStringLiteral("dolphin")
                                               << QStringLiteral("nemo")
                                               << QStringLiteral("thunar");
    for (const QString &manager : managers) {
        const QString executable = QStandardPaths::findExecutable(manager);
        if (executable.isEmpty())
            continue;
        const bool supported = (manager == QLatin1String("nautilus") || manager == QLatin1String("nemo"));
        if (supported
            && QProcess::startDetached(executable,
                                       QStringList() << QStringLiteral("--select") << nativePath)) {
            return true;
        }
        if (!supported
            && QProcess::startDetached(executable, QStringList() << nativePath)) {
            return true;
        }
    }

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        if (errorMessage)
            *errorMessage = QCoreApplication::translate("LaunchService", "无法打开所在目录：%1").arg(dir);
        return false;
    }
    return true;
#endif
}

} // namespace LaunchService
