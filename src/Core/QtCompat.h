#pragma once

#include <QtGlobal>

// Qt 5.7（Windows XP 兼容目标）只提供 IsEmpty/IsSet/IntValue 三个环境变量接口，
// 返回 QString 的 qEnvironmentVariable 是 Qt 5.10 才加入的，这里用 qgetenv 补上。
// 注意：qEnvironmentVariableIsEmpty 在 Qt 5.7 已存在，不能再重复定义。
#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)

#include <QByteArray>
#include <QString>

inline QString qEnvironmentVariable(const char *name)
{
    return QString::fromLocal8Bit(qgetenv(name));
}

#endif // QT_VERSION < 5.10（环境变量）

// QGuiApplication::screenAt 为 Qt 5.10 新增，Qt 5.7 下用 QDesktopWidget 实现等价语义
#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)

#include <QApplication>
#include <QCursor>
#include <QDesktopWidget>
#include <QGuiApplication>
#include <QScreen>

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

#endif // QT_VERSION < 5.10（screenAt）

#include <QChar>
#include <QString>
#include <QUuid>

// QUuid::WithoutBraces 为 Qt 5.11 新增，旧版本手动去掉花括号
inline QString listeryUuidString()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
#else
    QString text = QUuid::createUuid().toString();
    text.remove(QChar('{'));
    text.remove(QChar('}'));
    return text;
#endif
}

#include <QWidget>

// QWidget::setWindowFlag 为 Qt 5.9 新增，旧版本改写完整 flags
template <typename WidgetT>
inline void listerySetWindowFlag(WidgetT *widget, Qt::WindowType flag, bool on = true)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    widget->setWindowFlag(flag, on);
#else
    widget->setWindowFlags(on ? (widget->windowFlags() | flag)
                              : (widget->windowFlags() & ~Qt::WindowFlags(flag)));
#endif
}
