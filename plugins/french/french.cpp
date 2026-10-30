/*
 * Listery 法汉翻译插件 / French <-> Chinese translator plugin
 *
 * 输入法语给汉语，输入汉语给法语，回车把译文复制到剪贴板。
 * 实现见 plugins/common/translator_core.h（与英汉插件共用）。
 *
 * 数据源：有道词典 suggest 接口（明文 HTTP，le=fr，双向查词）。
 * 可用环境变量覆盖：LISTERY_FRENCH_ENDPOINT=youdao|google
 */

#define LISTERY_DICT_ID "com.listery.french"
#define LISTERY_DICT_NAME "法汉翻译"
#define LISTERY_DICT_DESCRIPTION "输入法语给汉语，输入汉语给法语；回车把译文复制到剪贴板"
#define LISTERY_DICT_HOTKEY "Alt+F"
#define LISTERY_DICT_BADGE "法"
#define LISTERY_DICT_LE_FOREIGN "fr"
#define LISTERY_DICT_GOOGLE_FOREIGN "fr"
#define LISTERY_DICT_LOG_NAME "french"
#define LISTERY_DICT_ENV "LISTERY_FRENCH_ENDPOINT"

#include "../common/translator_core.h"
