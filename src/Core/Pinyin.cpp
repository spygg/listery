#include "Core/Pinyin.h"

#include "Core/PinyinData.h"

#include <QVector>

namespace {
const int kMaxVariants = 6;

/** 去掉文件名的 ASCII 扩展名，避免把 "exe" 拼进拼音串。 */
QString stripExtension(const QString &name)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return name;

    const QString ext = name.mid(dot + 1);
    for (const QChar ch : ext) {
        if (ch.unicode() >= 128 || !ch.isLetterOrNumber())
            return name;
    }
    return name.left(dot);
}
} // namespace

namespace Pinyin {

void buildSearchForms(const QString &name, QString *fullPinyin, QString *initials)
{
    if (fullPinyin)
        fullPinyin->clear();
    if (initials)
        initials->clear();

    const QString base = stripExtension(name);

    struct Variant
    {
        QString full;
        QString shortForm;
    };
    QVector<Variant> variants;
    variants.append(Variant());
    bool hasHan = false;

    for (int i = 0; i < base.size();) {
        const QChar ch = base.at(i);
        if (PinyinData::isHan(ch.unicode())) {
            const QStringList readings = PinyinData::readings(ch.unicode());
            if (!readings.isEmpty()) {
                hasHan = true;
                QVector<Variant> expanded;
                expanded.reserve(kMaxVariants);
                for (const Variant &variant : variants) {
                    for (const QString &reading : readings) {
                        expanded.append(Variant{variant.full + reading,
                                                variant.shortForm + reading.left(1)});
                        if (expanded.size() >= kMaxVariants)
                            break;
                    }
                    if (expanded.size() >= kMaxVariants)
                        break;
                }
                variants = expanded;
            }
            ++i;
            continue;
        }

        if (ch.isLetterOrNumber()) {
            int end = i;
            while (end < base.size() && base.at(end).isLetterOrNumber()
                   && !PinyinData::isHan(base.at(end).unicode()))
                ++end;
            const QString word = base.mid(i, end - i).toLower();
            for (Variant &variant : variants) {
                variant.full += word;
                variant.shortForm += word.left(1);
            }
            i = end;
            continue;
        }

        // 空格、下划线、连字符等一律视作分隔符
        ++i;
    }

    if (!hasHan)
        return;

    QStringList fullList;
    QStringList shortList;
    for (const Variant &variant : variants) {
        if (variant.full.isEmpty() || fullList.contains(variant.full))
            continue;
        fullList.append(variant.full);
        shortList.append(variant.shortForm);
    }

    if (fullPinyin)
        *fullPinyin = fullList.join(QLatin1Char(' '));
    if (initials)
        *initials = shortList.join(QLatin1Char(' '));
}

} // namespace Pinyin
