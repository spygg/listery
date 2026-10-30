#pragma once

#include <QString>
#include <QStringList>

/**
 * 界面语言：简体中文（源语言）、繁體中文、English、Français。
 *
 * 翻译文件编译成 .qm 后嵌入资源（:/i18n/listery_<code>.qm），
 * 因此部署时只需要一个 exe，不需要额外的语言文件。
 */
namespace Localization {

/** 支持的语言代码，顺序即设置界面的显示顺序。 */
QStringList availableLanguages();

/** 语言的原生名称（用该语言本身书写），不会随界面语言变化。 */
QString nativeName(const QString &code);

/** 根据系统区域猜一个支持的语言；无法匹配时返回简体中文。 */
QString systemLanguage();

/**
 * 切换界面语言；返回是否真的发生了变化。
 * 安装 QTranslator 后 Qt 会向所有窗口发送 LanguageChange 事件。
 */
bool install(const QString &code);

/** 当前生效的语言代码。 */
QString current();

} // namespace Localization
