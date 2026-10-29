#pragma once

#include <QString>
#include <QThread>

/**
 * 双击 Ctrl 全局热键（Windows 键盘底层钩子）。
 *
 * 判定规则：
 *  - 两次 Ctrl 按下间隔 ≤ 设定阈值（默认 300ms）；
 *  - 两次之间必须完整松开过 Ctrl，且不能夹带其它按键（避免误判 Ctrl+C、Ctrl+V）；
 *  - 钩子回调里只做时间判断，通过队列连接把信号抛回 GUI 线程，绝不阻塞系统输入。
 */
class DoubleCtrlHook : public QThread
{
    Q_OBJECT

public:
    explicit DoubleCtrlHook(QObject *parent = nullptr);
    ~DoubleCtrlHook() override;

    void setIntervalMs(int milliseconds);
    void requestStop();

    static bool isSupported();

signals:
    void activated();
    void failed(const QString &reason);

protected:
    void run() override;

private slots:
    void emitActivated();

private:
    int m_intervalMs = 300;
    quintptr m_threadId = 0;
};
