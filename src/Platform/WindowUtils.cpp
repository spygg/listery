#include "Platform/WindowUtils.h"

#include <QCursor>
#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QWidget>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace WindowUtils {

void forceActivate(QWidget *widget)
{
    if (!widget)
        return;

#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    HWND foreground = GetForegroundWindow();
    DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    DWORD currentThread = GetCurrentThreadId();

    if (foregroundThread != 0 && foregroundThread != currentThread)
        AttachThreadInput(foregroundThread, currentThread, TRUE);

    // 先临时置顶再取消，绕过 Windows 的前台窗口限制
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (foregroundThread != 0 && foregroundThread != currentThread)
        AttachThreadInput(foregroundThread, currentThread, FALSE);
#else
    widget->raise();
#endif

    widget->raise();
    widget->activateWindow();
}

void placeCenterTop(QWidget *widget)
{
    if (!widget)
        return;

    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect available = screen->availableGeometry();
    const QSize size = widget->size();

    const int x = available.left() + (available.width() - size.width()) / 2;
    int y = available.top() + qMax(48, available.height() * 12 / 100);
    if (y + size.height() > available.bottom() - 8)
        y = available.top() + 24;

    widget->move(qMax(available.left(), x), qMax(available.top(), y));
}

void setAppUserModelId(const QString &appId)
{
#ifdef Q_OS_WIN
    // 某些 MinGW 头文件不声明该函数，这里动态取地址，避免依赖 SDK 版本
    typedef HRESULT(WINAPI * SetAppUserModelIdFn)(PCWSTR);
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32)
        shell32 = LoadLibraryW(L"shell32.dll");
    if (!shell32)
        return;

    const auto setAppId = reinterpret_cast<SetAppUserModelIdFn>(
        reinterpret_cast<void *>(GetProcAddress(shell32, "SetCurrentProcessExplicitAppUserModelID")));
    if (setAppId)
        setAppId(reinterpret_cast<const wchar_t *>(appId.utf16()));
#else
    Q_UNUSED(appId)
#endif
}

} // namespace WindowUtils
