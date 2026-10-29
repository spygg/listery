#pragma once

#include <QString>
#include <QStringList>

/** 汉字读音表（由 tools/gen_pinyin_data.py 生成的数据文件提供）。 */
namespace PinyinData {

bool isHan(ushort codePoint);

/** 返回该汉字的读音（主读音在前，unicode 之外与多音字之外都返回空/单元素）。 */
QStringList readings(ushort codePoint);

} // namespace PinyinData
