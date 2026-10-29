#pragma once

#include <QtGlobal>

/**
 * 双击 Ctrl 判定状态机（与平台无关，便于单元测试）。
 *
 * 触发条件：
 *  1. 两次 Ctrl 按下之间的间隔不超过阈值；
 *  2. 两次之间 Ctrl 必须完整抬起过；
 *  3. 按住 Ctrl 期间不能夹带其它按键（避免把 Ctrl+C、Ctrl+V 误判成唤起）。
 */
class DoubleCtrlDetector
{
public:
    void setIntervalMs(int milliseconds)
    {
        m_intervalMs = qBound(150, milliseconds, 800);
    }

    int intervalMs() const
    {
        return m_intervalMs;
    }

    void reset()
    {
        m_lastPressMs = 0;
        m_ctrlHeld = false;
        m_otherKeyWhileCtrl = false;
    }

    /** 喂入一次按键事件；返回 true 表示此刻应当唤起搜索框。 */
    bool handleKey(bool isCtrl, bool isDown, qint64 nowMs)
    {
        if (isCtrl) {
            if (isDown) {
                if (m_ctrlHeld)
                    return false; // 长按产生的重复 keydown
                m_ctrlHeld = true;

                if (m_lastPressMs != 0 && nowMs >= m_lastPressMs
                    && nowMs - m_lastPressMs <= m_intervalMs && !m_otherKeyWhileCtrl) {
                    m_lastPressMs = 0;
                    m_otherKeyWhileCtrl = false;
                    return true;
                }

                m_lastPressMs = nowMs;
                m_otherKeyWhileCtrl = false;
                return false;
            }

            m_ctrlHeld = false;
            return false;
        }

        if (isDown && m_ctrlHeld)
            m_otherKeyWhileCtrl = true;
        return false;
    }

private:
    int m_intervalMs = 300;
    qint64 m_lastPressMs = 0;
    bool m_ctrlHeld = false;
    bool m_otherKeyWhileCtrl = false;
};
