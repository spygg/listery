/*
 * Listery 英汉翻译插件 / English <-> Chinese translator plugin
 *
 * 输入英文给汉语，输入汉语给英文，回车把译文复制到剪贴板。
 * 实现见 plugins/common/translator_core.h（与法汉插件共用）。
 *
 * 数据源：有道词典 suggest 接口（明文 HTTP，中英双向）。
 * 可用环境变量覆盖：LISTERY_TRANSLATOR_ENDPOINT=youdao|google
 */

#define LISTERY_DICT_ID "com.listery.translator"
#define LISTERY_DICT_NAME "英汉翻译"
#define LISTERY_DICT_DESCRIPTION "输入英文给汉语，输入汉语给英文；回车把译文复制到剪贴板"
#define LISTERY_DICT_HOTKEY "Alt+E"
#define LISTERY_DICT_BADGE "译"
#define LISTERY_DICT_LE_FOREIGN "en"
#define LISTERY_DICT_GOOGLE_FOREIGN "en"
#define LISTERY_DICT_LOG_NAME "translator"
#define LISTERY_DICT_ENV "LISTERY_TRANSLATOR_ENDPOINT"

#include "../common/translator_core.h"
