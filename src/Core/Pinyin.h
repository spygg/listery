#pragma once

#include <QString>
#include <QStringList>

namespace Pinyin {

/**
 * 把文件名里的汉字转成用于检索的拼音串。
 *
 * 全拼取主读音，多音字最多保留 6 种组合（主读音组合优先），用空格分隔存放，
 * 这样 "zhaoshangyinhang" 与 "zhaoshangyinxing" 都能命中；首字母串与全拼一一对应。
 * 名字里没有汉字时不生成（避免给纯英文程序白占索引）。
 */
void buildSearchForms(const QString &name, QString *fullPinyin, QString *initials);

} // namespace Pinyin
