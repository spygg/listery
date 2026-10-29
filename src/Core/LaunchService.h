#pragma once

#include "Core/Database.h"

#include <QString>

/**
 * 启动服务：把选中的可执行文件交给系统启动。
 * 支持：普通启动、以管理员身份启动、在资源管理器中定位。
 */
namespace LaunchService {

bool launch(const ExecutableRecord &record, bool asAdmin, QString *errorMessage = nullptr);

bool revealInFolder(const QString &filePath, QString *errorMessage = nullptr);

/** 返回当前平台上双击 Ctrl 之外的兜底说明，用于界面提示。 */
QString platformHint();

} // namespace LaunchService
