#pragma once

#include <QtGlobal>

// Windows XP 兼容目标使用 Qt 5.7：以下接口在 Qt 5.10 才引入，
// 用 qgetenv 提供等价实现，5.10+ 编译时保持原生行为。
#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)

#include <QByteArray>
#include <QString>

inline QString qEnvironmentVariable(const char *name)
{
    return QString::fromLocal8Bit(qgetenv(name));
}

inline bool qEnvironmentVariableIsEmpty(const char *name)
{
    return qgetenv(name).isEmpty();
}

#include <QApplication>
#include <QCursor>
#include <QDesktopWidget>
#include <QGuiApplication>
#include <QScreen>

// QGuiApplication::screenAt 为 Qt 5.10 新增，Qt 5.7 下用 QDesktopWidget 等价实现
inline QScreen *listeryScreenAtCursor()
{
    const int index = QApplication::desktop()->screenNumber(QCursor::pos());
    return QGuiApplication::screens().value(index);
}

#else

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

inline QScreen *listeryScreenAtCursor()
{
    return QGuiApplication::screenAt(QCursor::pos());
}

#endif // QT_VERSION < 5.10
