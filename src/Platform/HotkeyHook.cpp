#include "Platform/HotkeyHook.h"

#include "Platform/DoubleCtrlDetector.h"
#include "Core/QtCompat.h"

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <chrono>

namespace {
qint64 monotonicMilliseconds()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

QMutex g_hotkeyMutex;
QVector<HotkeySpec> g_hotkeys;

QString matchHotkey(unsigned int mods, int key)
{
    if (key == 0)
        return QString();
    QMutexLocker locker(&g_hotkeyMutex);
    for (const HotkeySpec &spec : g_hotkeys) {
        if (hotkeyMatches(spec, mods, key))
            return spec.text;
    }
    return QString();
}

struct KeyNameEntry
{
    const char *name;
    int key;
};

const KeyNameEntry kKeyNames[] = {
    {"A", Qt::Key_A}, {"B", Qt::Key_B}, {"C", Qt::Key_C}, {"D", Qt::Key_D},
    {"E", Qt::Key_E}, {"F", Qt::Key_F}, {"G", Qt::Key_G}, {"H", Qt::Key_H},
    {"I", Qt::Key_I}, {"J", Qt::Key_J}, {"K", Qt::Key_K}, {"L", Qt::Key_L},
    {"M", Qt::Key_M}, {"N", Qt::Key_N}, {"O", Qt::Key_O}, {"P", Qt::Key_P},
    {"Q", Qt::Key_Q}, {"R", Qt::Key_R}, {"S", Qt::Key_S}, {"T", Qt::Key_T},
    {"U", Qt::Key_U}, {"V", Qt::Key_V}, {"W", Qt::Key_W}, {"X", Qt::Key_X},
    {"Y", Qt::Key_Y}, {"Z", Qt::Key_Z},
    {"0", Qt::Key_0}, {"1", Qt::Key_1}, {"2", Qt::Key_2}, {"3", Qt::Key_3},
    {"4", Qt::Key_4}, {"5", Qt::Key_5}, {"6", Qt::Key_6}, {"7", Qt::Key_7},
    {"8", Qt::Key_8}, {"9", Qt::Key_9},
    {"F1", Qt::Key_F1}, {"F2", Qt::Key_F2}, {"F3", Qt::Key_F3}, {"F4", Qt::Key_F4},
    {"F5", Qt::Key_F5}, {"F6", Qt::Key_F6}, {"F7", Qt::Key_F7}, {"F8", Qt::Key_F8},
    {"F9", Qt::Key_F9}, {"F10", Qt::Key_F10}, {"F11", Qt::Key_F11}, {"F12", Qt::Key_F12},
    {"SPACE", Qt::Key_Space}, {"ENTER", Qt::Key_Return}, {"RETURN", Qt::Key_Return},
    {"TAB", Qt::Key_Tab}, {"ESC", Qt::Key_Escape}, {"ESCAPE", Qt::Key_Escape},
    {"HOME", Qt::Key_Home}, {"END", Qt::Key_End}, {"INSERT", Qt::Key_Insert},
    {"DEL", Qt::Key_Delete}, {"DELETE", Qt::Key_Delete},
};

int keyFromName(const QString &name)
{
    const QString upper = name.trimmed().toUpper();
    if (upper.isEmpty())
        return 0;
    for (const KeyNameEntry &entry : kKeyNames) {
        if (upper == QString::fromLatin1(entry.name))
            return entry.key;
    }
    return 0;
}

QString nameFromKey(int key)
{
    for (const KeyNameEntry &entry : kKeyNames) {
        if (entry.key == key)
            return QString::fromLatin1(entry.name);
    }
    return QString();
}

QString modifierLabel(const HotkeySpec &spec)
{
    QStringList parts;
    if (spec.ctrl)
        parts << QStringLiteral("Ctrl");
    if (spec.alt)
        parts << QStringLiteral("Alt");
    if (spec.shift)
        parts << QStringLiteral("Shift");
    if (spec.meta)
        parts << QStringLiteral("Meta");
    return parts.join(QStringLiteral("+"));
}
} // namespace

bool hotkeyMatches(const HotkeySpec &spec, unsigned int modifiers, int key)
{
    if (!spec.valid() || spec.key != key)
        return false;
    unsigned int want = 0;
    if (spec.ctrl)
        want |= ModCtrl;
    if (spec.alt)
        want |= ModAlt;
    if (spec.shift)
        want |= ModShift;
    if (spec.meta)
        want |= ModMeta;
    return want == modifiers;
}

HotkeySpec parseHotkey(const QString &text)
{
    HotkeySpec spec;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QStringList parts = text.split(QLatin1Char('+'), Qt::KeepEmptyParts);
#else
    const QStringList parts = text.split(QLatin1Char('+'), QString::KeepEmptyParts);
#endif
    for (int i = 0; i < parts.size(); ++i) {
        const QString token = parts.at(i).trimmed().toLower();
        if (token.isEmpty())
            continue;
        if (token == QStringLiteral("ctrl") || token == QStringLiteral("control")) {
            spec.ctrl = true;
            continue;
        }
        if (token == QStringLiteral("alt") || token == QStringLiteral("option")) {
            spec.alt = true;
            continue;
        }
        if (token == QStringLiteral("shift")) {
            spec.shift = true;
            continue;
        }
        if (token == QStringLiteral("meta") || token == QStringLiteral("win")
            || token == QStringLiteral("cmd") || token == QStringLiteral("command")
            || token == QStringLiteral("super")) {
            spec.meta = true;
            continue;
        }
        spec.key = keyFromName(token);
    }

    if (!spec.valid())
        return HotkeySpec();
    spec.text = text.trimmed();
    return spec;
}

QString normalizeHotkey(const QString &text)
{
    HotkeySpec spec = parseHotkey(text);
    if (!spec.valid())
        return QString();

    QStringList parts;
    const QString mods = modifierLabel(spec);
    if (!mods.isEmpty())
        parts << mods;
    parts << nameFromKey(spec.key);
    return parts.join(QStringLiteral("+"));
}

// ============================ Windows ============================
#ifdef Q_OS_WIN
#  include <windows.h>

namespace {
GlobalHotkeys *g_hook = nullptr;
DoubleCtrlDetector g_detector;
std::atomic<int> g_intervalMs{300};
std::atomic<bool> g_allowInjected{false};

int windowsKeyToQt(DWORD vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return Qt::Key_A + static_cast<int>(vk - 'A');
    if (vk >= '0' && vk <= '9')
        return Qt::Key_0 + static_cast<int>(vk - '0');
    if (vk >= VK_F1 && vk <= VK_F24)
        return Qt::Key_F1 + static_cast<int>(vk - VK_F1);
    switch (vk) {
    case VK_SPACE:
        return Qt::Key_Space;
    case VK_RETURN:
        return Qt::Key_Return;
    case VK_TAB:
        return Qt::Key_Tab;
    case VK_ESCAPE:
        return Qt::Key_Escape;
    case VK_HOME:
        return Qt::Key_Home;
    case VK_END:
        return Qt::Key_End;
    case VK_INSERT:
        return Qt::Key_Insert;
    case VK_DELETE:
        return Qt::Key_Delete;
    default:
        break;
    }
    return 0;
}

unsigned int currentModifiers()
{
    unsigned int mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        mods |= ModCtrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)
        mods |= ModAlt;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        mods |= ModShift;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
        mods |= ModMeta;
    return mods;
}

LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && lParam != 0 && g_hook != nullptr) {
        const KBDLLHOOKSTRUCT *info = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
        const bool injected = (info->flags & LLKHF_INJECTED) != 0;
        if (!injected || g_allowInjected.load()) {
            const DWORD vk = info->vkCode;
            const bool isCtrl = (vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL);
            const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

            bool consume = false;
            if (isDown) {
                const int key = windowsKeyToQt(vk);
                if (key != 0) {
                    const QString hit = matchHotkey(currentModifiers(), key);
                    if (!hit.isEmpty()) {
                        // 命中插件热键（如 Alt+E）：吞掉这次按键，避免 e 等字符落到
                        // 当前聚焦的输入框。注意只吞「最终按键」，修饰键（Ctrl/Alt/Shift）
                        // 仍放行，否则会破坏其它应用的正常组合键（如 Ctrl+C）。
                        consume = true;
                        QMetaObject::invokeMethod(g_hook, "emitHotkey", Qt::QueuedConnection,
                                                  Q_ARG(QString, hit));
                    }
                }
            }

            if (isCtrl || isDown) {
                g_detector.setIntervalMs(g_intervalMs.load());
                if (g_detector.handleKey(isCtrl, isDown, monotonicMilliseconds()))
                    QMetaObject::invokeMethod(g_hook, "emitActivated", Qt::QueuedConnection);
            }

            if (consume)
                return 1; // 拦截：命中插件的按键不再传给目标窗口
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
GlobalHotkeys *g_hook = nullptr;
DoubleCtrlDetector g_detector;
std::atomic<int> g_intervalMs{300};
std::atomic<CFRunLoopRef> g_runLoop{nullptr};
std::atomic<bool> g_stopRequested{false};

/** 常见的 macOS 虚拟键码 → Qt::Key（字母、数字与 F1-F12，够插件热键用）。 */
int macKeyCodeToQt(CGKeyCode code)
{
    static const int kLetterMap[47] = {
        Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_F, Qt::Key_H, Qt::Key_G, Qt::Key_Z, Qt::Key_X,
        Qt::Key_C, Qt::Key_V, 0, Qt::Key_B, Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R,
        Qt::Key_Y, Qt::Key_T, Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_6,
        Qt::Key_5, Qt::Key_Equal, Qt::Key_9, Qt::Key_7, Qt::Key_Minus, Qt::Key_8, Qt::Key_0,
        Qt::Key_BracketRight, Qt::Key_O, Qt::Key_U, Qt::Key_BracketLeft, Qt::Key_I, Qt::Key_P,
        Qt::Key_Return, Qt::Key_L, Qt::Key_J, Qt::Key_QuoteDbl, Qt::Key_K, Qt::Key_Semicolon,
        Qt::Key_Backslash, Qt::Key_Comma, Qt::Key_Slash, Qt::Key_N, Qt::Key_M
    };
    if (code < sizeof(kLetterMap) / sizeof(kLetterMap[0]))
        return kLetterMap[code];
    if (code >= 122 && code <= 133) // kVK_F1 .. kVK_F12
        return Qt::Key_F1 + static_cast<int>(code - 122);
    if (code == 49)
        return Qt::Key_Space;
    if (code == 48)
        return Qt::Key_Tab;
    if (code == 53)
        return Qt::Key_Escape;
    return 0;
}

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

        const CGEventFlags flags = CGEventGetFlags(event);
        unsigned int mods = 0;
        if (flags & kCGEventFlagMaskControl)
            mods |= ModCtrl;
        if (flags & kCGEventFlagMaskAlternate)
            mods |= ModAlt;
        if (flags & kCGEventFlagMaskShift)
            mods |= ModShift;
        if (flags & kCGEventFlagMaskCommand)
            mods |= ModMeta;

        const CGKeyCode keyCode =
            static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
        const int key = macKeyCodeToQt(keyCode);
        const QString hit = matchHotkey(mods, key);
        if (!hit.isEmpty())
            QMetaObject::invokeMethod(g_hook, "emitHotkey", Qt::QueuedConnection,
                                      Q_ARG(QString, hit));
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
GlobalHotkeys *g_hook = nullptr;
DoubleCtrlDetector g_detector;
std::atomic<int> g_intervalMs{300};
std::atomic<bool> g_stopRequested{false};
std::atomic<unsigned int> g_mods{0}; // 自己维护修饰键状态：XRecord 回传的事件里没有状态位

Display *g_ctrlDisplay = nullptr; // 主线程用于停止
Display *g_dataDisplay = nullptr; // 回调线程专用
XRecordContext g_context = 0;

int xKeySymToQt(KeySym sym)
{
    if (sym >= XK_a && sym <= XK_z)
        return Qt::Key_A + static_cast<int>(sym - XK_a);
    if (sym >= XK_A && sym <= XK_Z)
        return Qt::Key_A + static_cast<int>(sym - XK_A);
    if (sym >= XK_0 && sym <= XK_9)
        return Qt::Key_0 + static_cast<int>(sym - XK_0);
    if (sym >= XK_F1 && sym <= XK_F35)
        return Qt::Key_F1 + static_cast<int>(sym - XK_F1);
    switch (sym) {
    case XK_space:
        return Qt::Key_Space;
    case XK_Return:
        return Qt::Key_Return;
    case XK_Tab:
        return Qt::Key_Tab;
    case XK_Escape:
        return Qt::Key_Escape;
    case XK_Home:
        return Qt::Key_Home;
    case XK_End:
        return Qt::Key_End;
    case XK_Insert:
        return Qt::Key_Insert;
    case XK_Delete:
        return Qt::Key_Delete;
    default:
        break;
    }
    return 0;
}

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
            const bool isDown = (type == KeyPress);

            unsigned int mods = g_mods.load();
            switch (sym) {
            case XK_Control_L:
            case XK_Control_R:
                mods = isDown ? (mods | ModCtrl) : (mods & ~ModCtrl);
                g_mods.store(mods);
                break;
            case XK_Alt_L:
            case XK_Alt_R:
                mods = isDown ? (mods | ModAlt) : (mods & ~ModAlt);
                g_mods.store(mods);
                break;
            case XK_Shift_L:
            case XK_Shift_R:
                mods = isDown ? (mods | ModShift) : (mods & ~ModShift);
                g_mods.store(mods);
                break;
            case XK_Super_L:
            case XK_Super_R:
                mods = isDown ? (mods | ModMeta) : (mods & ~ModMeta);
                g_mods.store(mods);
                break;
            default:
                break;
            }

            const bool isCtrl = (sym == XK_Control_L || sym == XK_Control_R);
            g_detector.setIntervalMs(g_intervalMs.load());
            if (g_detector.handleKey(isCtrl, isDown, monotonicMilliseconds()))
                QMetaObject::invokeMethod(g_hook, "emitActivated", Qt::QueuedConnection);

            if (isDown && !isCtrl) {
                const int key = xKeySymToQt(sym);
                const QString hit = matchHotkey(g_mods.load(), key);
                if (!hit.isEmpty())
                    QMetaObject::invokeMethod(g_hook, "emitHotkey", Qt::QueuedConnection,
                                              Q_ARG(QString, hit));
            }
        }
    }
    XRecordFreeData(data);
}
} // namespace
#endif

GlobalHotkeys::GlobalHotkeys(QObject *parent)
    : QThread(parent)
{
}

GlobalHotkeys::~GlobalHotkeys()
{
    requestStop();
    if (isRunning())
        wait(2000);
}

void GlobalHotkeys::setIntervalMs(int milliseconds)
{
    m_intervalMs = qBound(150, milliseconds, 800);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || (defined(Q_OS_LINUX) && defined(LISTERY_HAVE_X11))
    g_intervalMs.store(m_intervalMs);
#endif
}

void GlobalHotkeys::setHotkeys(const QVector<HotkeySpec> &specs)
{
    QMutexLocker locker(&g_hotkeyMutex);
    g_hotkeys = specs;
}

bool GlobalHotkeys::isSupported()
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

void GlobalHotkeys::emitActivated()
{
    emit activated();
}

void GlobalHotkeys::emitHotkey(const QString &text)
{
    emit hotkeyTriggered(text);
}

void GlobalHotkeys::requestStop()
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

void GlobalHotkeys::run()
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
    qWarning("Listery: 当前平台未实现系统级全局按键监听");
#endif
}
