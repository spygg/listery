#pragma once

#include <QString>

/** 开机自启动：写入 HKCU\Software\Microsoft\Windows\CurrentVersion\Run。 */
namespace Autostart {

bool isSupported();

/** 注册表里是否存在本程序的自启动项。 */
bool isEnabled();

/** 写入或删除自启动项；命令行始终指向当前 exe，移动目录后再次调用即可修正。 */
bool setEnabled(bool on, QString *error = nullptr);

/** 当前程序应写入的启动命令行。 */
QString commandLine();

} // namespace Autostart
