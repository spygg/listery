#pragma once

#include <QString>

class QWidget;

namespace WindowUtils {

/** 把无边框工具窗口强行切到前台并获得键盘焦点（Windows 下带 AttachThreadInput 兜底）。 */
void forceActivate(QWidget *widget);

/** 在鼠标所在屏幕水平居中、垂直靠上（约屏幕高度 12% 处）弹出。 */
void placeCenterTop(QWidget *widget);

/** 让任务栏/托盘显示正确名称（Windows AppUserModelID）。 */
void setAppUserModelId(const QString &appId);

} // namespace WindowUtils
