#pragma once

#include <QString>
#include <QThread>
#include <QVector>

/**
 * 全局组合键描述（与平台无关的中间表示）。
 *
 * text 是人类可读形式，如 "Alt+E"；key 用 Qt::Key 的数值表示（避免头文件依赖 Qt 枚举）。
 * 修饰键要求严格相等：登记了 Alt+E，则 Ctrl+Alt+E 不会触发。
 */
struct HotkeySpec
{
    QString text;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;
    int key = 0;

    bool valid() const { return key != 0; }
};

/** 修饰键位（与平台回调里维护的状态一致）。 */
enum HotkeyModifier
{
    ModCtrl = 1,
    ModAlt = 2,
    ModShift = 4,
    ModMeta = 8
};

/**
 * 一次按键是否命中某个登记的热键（修饰键严格相等）。
 * 平台回调与自检共用这一份判定，避免三份实现各自漂移。
 */
bool hotkeyMatches(const HotkeySpec &spec, unsigned int modifiers, int key);

/** 解析 "Alt+E" / "Ctrl+Shift+F1" 等写法；无法识别时返回 key == 0 的规格。 */
HotkeySpec parseHotkey(const QString &text);

/** 把用户随意的写法整理成规范形式（"alt + e" → "Alt+E"），无法识别时返回空串。 */
QString normalizeHotkey(const QString &text);

/**
 * 全局热键监听：既负责双击 Ctrl，也负责插件注册的 Alt+E 这类组合键。
 *
 * 三平台实现：
 *  - Windows：WH_KEYBOARD_LL 低级键盘钩子；
 *  - macOS：CGEventTap（需要“辅助功能 / 输入监控”权限）；
 *  - Linux：X11 XRecord（Wayland 会话不可用）。
 * 回调里只做时间/按键判断，命中后通过队列连接把信号抛回 GUI 线程，绝不阻塞系统输入。
 */
class GlobalHotkeys : public QThread
{
    Q_OBJECT

public:
    explicit GlobalHotkeys(QObject *parent = nullptr);
    ~GlobalHotkeys() override;

    void setIntervalMs(int milliseconds);
    /** 更新组合键列表（主线程调用；内部加锁，回调线程只读）。 */
    void setHotkeys(const QVector<HotkeySpec> &specs);
    void requestStop();

    static bool isSupported();

signals:
    /** 双击 Ctrl 命中。 */
    void activated();
    /** 组合键命中，text 为登记的原始写法（如 "Alt+E"）。 */
    void hotkeyTriggered(const QString &text);
    void failed(const QString &reason);

protected:
    void run() override;

private slots:
    void emitActivated();
    /** 由钩子线程通过队列连接调用，把命中转发成 hotkeyTriggered 信号。 */
    void emitHotkey(const QString &text);

private:
    int m_intervalMs = 300;
    quintptr m_threadId = 0;
};
