#include "Platform/HotkeyHook.h"

#include "Platform/DoubleCtrlDetector.h"

#include <QDebug>
#include <QThread>

#include <atomic>
#include <chrono>

namespace {
qint64 monotonicMilliseconds()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

// ============================ Windows ============================
#ifdef Q_OS_WIN
#  include <windows.h>

namespace {
DoubleCtrlHook *g_hook = nullptr;
DoubleCtrlDetector g_detector;
std::atomic<int> g_intervalMs{300};
std::atomic<bool> g_allowInjected{false};

LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && lParam != 0 && g_hook != nullptr) {
        const KBDLLHOOKSTRUCT *info = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
        const bool injected = (info->flags & LLKHF_INJECTED) != 0;
        if (!injected || g_allowInjected.load()) {
            const DWORD vk = info->vkCode;
            const bool isCtrl = (vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL);
            const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

            if (isCtrl || isDown) {
                g_detector.setIntervalMs(g_intervalMs.load());
                if (g_detector.handleKey(isCtrl, isDown, monotonicMilliseconds()))
                    QMetaObject::invokeMethod(g_hook, "emitActivated", Qt::QueuedConnection);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
} // namespace
#endif

// ============================= macOS =============================
#ifdef Q_OS_MACOS
#  include <ApplicationServices/ApplicationServices.h>

namespace {
DoubleCtrlHook *g_hook = nullptr;
DoubleCtrlDetector g_detector;
std::atomic<int> g_intervalMs{300};
std::atomic<CFRunLoopRef> g_runLoop{nullptr};
std::atomic<bool> g_stopRequested{false};

CGEventRef eventTapCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon)
{
    Q_UNUSED(proxy)
    Q_UNUSED(refcon)
    if (g_hook == nullptr)
        return event;

    bool isCtrl = false;
    bool isDown = false;

    if (type == kCGEventFlagsChanged) {
        const CGKeyCode keyCode =
            static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
        if (keyCode == 59 || keyCode == 62) { // 左 / 右 Control
            isCtrl = true;
            isDown = (CGEventGetFlags(event) & kCGEventFlagMaskControl) != 0;
        } else {
            // 其它修饰键按下也视为“夹带按键”，让状态机有机会复位
            isDown = true;
        }
    } else if (type == kCGEventKeyDown) {
        isDown = true;
    } else if (type == kCGEventKeyUp) {
        isDown = false;
    } else {
        return event;
    }

    g_detector.setIntervalMs(g_intervalMs.load());
    if (g_detector.handleKey(isCtrl, isDown, monotonicMilliseconds()))
        QMetaObject::invokeMethod(g_hook, "emitActivated", Qt::QueuedConnection);

    return event;
}
} // namespace
#endif

// ======================= Linux (X11 / XRecord) ====================
#if defined(Q_OS_LINUX) && defined(LISTERY_HAVE_X11)
#  include <X11/XKBlib.h>
#  include <X11/Xlib.h>
#  include <X11/extensions/record.h>
#  include <X11/keysym.h>

namespace {
DoubleCtrlHook *g_hook = nullptr;
DoubleCtrlDetector g_detector;
std::atomic<int> g_intervalMs{300};
std::atomic<bool> g_stopRequested{false};

Display *g_ctrlDisplay = nullptr; // 主线程用于停止
Display *g_dataDisplay = nullptr; // 回调线程专用
XRecordContext g_context = 0;

void recordCallback(XPointer closure, XRecordInterceptData *data)
{
    Q_UNUSED(closure)
    if (data == nullptr)
        return;
    if (data->category == XRecordFromServer && g_hook != nullptr) {
        // XRecord 回传的是协议原始事件，首字节为事件类型、第二字节为按键码
        const unsigned char *raw = reinterpret_cast<const unsigned char *>(data->data);
        const unsigned char type = raw[0];
        if (type == KeyPress || type == KeyRelease) {
            const unsigned char keyCode = raw[1];
            const KeySym sym = XkbKeycodeToKeysym(g_dataDisplay, keyCode, 0, 0);
            const bool isCtrl = (sym == XK_Control_L || sym == XK_Control_R);
            const bool isDown = (type == KeyPress);
            g_detector.setIntervalMs(g_intervalMs.load());
            if (g_detector.handleKey(isCtrl, isDown, monotonicMilliseconds()))
                QMetaObject::invokeMethod(g_hook, "emitActivated", Qt::QueuedConnection);
        }
    }
    XRecordFreeData(data);
}
} // namespace
#endif

DoubleCtrlHook::DoubleCtrlHook(QObject *parent)
    : QThread(parent)
{
}

DoubleCtrlHook::~DoubleCtrlHook()
{
    requestStop();
    if (isRunning())
        wait(2000);
}

void DoubleCtrlHook::setIntervalMs(int milliseconds)
{
    m_intervalMs = qBound(150, milliseconds, 800);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || (defined(Q_OS_LINUX) && defined(LISTERY_HAVE_X11))
    g_intervalMs.store(m_intervalMs);
#endif
}

bool DoubleCtrlHook::isSupported()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return true;
#elif defined(Q_OS_LINUX) && defined(LISTERY_HAVE_X11)
    // XRecord 只在 X11 会话里有效；Wayland 会话拿不到全局按键
    const QString session = qEnvironmentVariable("XDG_SESSION_TYPE");
    if (session.compare(QStringLiteral("wayland"), Qt::CaseInsensitive) == 0)
        return false;
    return !qEnvironmentVariableIsEmpty("DISPLAY");
#else
    return false;
#endif
}

void DoubleCtrlHook::emitActivated()
{
    emit activated();
}

void DoubleCtrlHook::requestStop()
{
#ifdef Q_OS_WIN
    if (m_threadId != 0)
        PostThreadMessageW(static_cast<DWORD>(m_threadId), WM_QUIT, 0, 0);
#elif defined(Q_OS_MACOS)
    g_stopRequested.store(true);
    if (CFRunLoopRef loop = g_runLoop.load())
        CFRunLoopStop(loop);
#elif defined(Q_OS_LINUX) && defined(LISTERY_HAVE_X11)
    g_stopRequested.store(true);
    if (g_ctrlDisplay != nullptr && g_context != 0) {
        XRecordDisableContext(g_ctrlDisplay, g_context);
        XFlush(g_ctrlDisplay);
    }
#endif
}

void DoubleCtrlHook::run()
{
#ifdef Q_OS_WIN
    m_threadId = static_cast<quintptr>(GetCurrentThreadId());

    // 先创建线程消息队列，保证 requestStop() 的 PostThreadMessage 可靠
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    g_intervalMs.store(m_intervalMs);
    g_allowInjected.store(!qEnvironmentVariableIsEmpty("LISTERY_TEST_ALLOW_INJECTED"));
    g_detector.reset();
    g_detector.setIntervalMs(m_intervalMs);
    g_hook = this;

    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyboardProc,
                                   GetModuleHandleW(nullptr), 0);
    if (hook == nullptr) {
        g_hook = nullptr;
        const QString reason = QStringLiteral("SetWindowsHookEx 键盘钩子安装失败，错误码 %1")
                                   .arg(static_cast<qulonglong>(GetLastError()));
        qWarning("Listery: %s", qPrintable(reason));
        emit failed(reason);
        return;
    }

    int result = 0;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(hook);
    g_hook = nullptr;
#elif defined(Q_OS_MACOS)
    g_intervalMs.store(m_intervalMs);
    g_stopRequested.store(false);
    g_detector.reset();
    g_detector.setIntervalMs(m_intervalMs);
    g_hook = this;

    const CGEventMask mask = CGEventMaskBit(kCGEventFlagsChanged)
        | CGEventMaskBit(kCGEventKeyDown)
        | CGEventMaskBit(kCGEventKeyUp);
    CFMachPortRef tap = CGEventTapCreate(kCGSessionEventTap,
                                         kCGHeadInsertEventTap,
                                         kCGEventTapOptionListenOnly,
                                         mask,
                                         eventTapCallback,
                                         nullptr);
    if (tap == nullptr) {
        g_hook = nullptr;
        const QString reason = QStringLiteral("无法创建键盘监听，请在“系统设置 → 隐私与安全性 → 辅助功能/输入监控”里给 Listery 授权");
        qWarning("Listery: %s", qPrintable(reason));
        emit failed(reason);
        return;
    }

    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    g_runLoop.store(loop);
    CFRunLoopAddSource(loop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    if (!g_stopRequested.load())
        CFRunLoopRun();

    g_runLoop.store(nullptr);
    CFRelease(source);
    CFRelease(tap);
    g_hook = nullptr;
#elif defined(Q_OS_LINUX) && defined(LISTERY_HAVE_X11)
    g_intervalMs.store(m_intervalMs);
    g_stopRequested.store(false);
    g_detector.reset();
    g_detector.setIntervalMs(m_intervalMs);

    Display *ctrlDisplay = XOpenDisplay(nullptr);
    if (ctrlDisplay == nullptr) {
        emit failed(QStringLiteral("无法连接 X11 显示服务（DISPLAY 为空或不可访问）"));
        return;
    }

    int major = 0;
    int minor = 0;
    if (!XRecordQueryVersion(ctrlDisplay, &major, &minor)) {
        XCloseDisplay(ctrlDisplay);
        emit failed(QStringLiteral("X11 缺少 XRecord 扩展，无法监听全局按键"));
        return;
    }

    Display *dataDisplay = XOpenDisplay(nullptr);
    if (dataDisplay == nullptr) {
        XCloseDisplay(ctrlDisplay);
        emit failed(QStringLiteral("无法打开 X11 数据连接"));
        return;
    }

    XRecordRange *range = XRecordAllocRange();
    range->device_events.first = KeyPress;
    range->device_events.last = KeyRelease;
    XRecordClientSpec clients = XRecordAllClients;
    XRecordContext context = XRecordCreateContext(ctrlDisplay, 0, &clients, 1, &range, 1);
    XFree(range);
    if (context == 0) {
        XCloseDisplay(dataDisplay);
        XCloseDisplay(ctrlDisplay);
        emit failed(QStringLiteral("创建 XRecord 上下文失败"));
        return;
    }

    g_ctrlDisplay = ctrlDisplay;
    g_dataDisplay = dataDisplay;
    g_context = context;
    g_hook = this;

    if (!g_stopRequested.load())
        XRecordEnableContext(dataDisplay, context, recordCallback, nullptr);

    g_hook = nullptr;
    g_context = 0;
    g_dataDisplay = nullptr;
    g_ctrlDisplay = nullptr;
    XRecordFreeContext(ctrlDisplay, context);
    XCloseDisplay(dataDisplay);
    XCloseDisplay(ctrlDisplay);
#else
    qWarning("Listery: 当前平台未实现系统级双击 Ctrl 监听");
#endif
}
