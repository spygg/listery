#include "Core/Localization.h"

#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>

namespace {

/** 源语言：所有 tr() 里的字符串都是简体中文，不需要 .qm。 */
const char *kSourceLanguage = "zh_CN";

QString g_current;
QTranslator *g_translator = nullptr;

/** 把系统区域名或用户输入归一到四种支持的语言代码；无法识别时返回空串。 */
QString normalize(const QString &value)
{
    const QString code = value.trimmed().replace(QLatin1Char('-'), QLatin1Char('_'));
    if (code.isEmpty())
        return QString();

    const QString lower = code.toLower();
    if (lower == QLatin1String("zh_cn") || lower == QLatin1String("zh_hans")
        || lower == QLatin1String("zh_sg"))
        return QStringLiteral("zh_CN");
    if (lower == QLatin1String("zh_tw") || lower == QLatin1String("zh_hk")
        || lower == QLatin1String("zh_mo") || lower == QLatin1String("zh_hant"))
        return QStringLiteral("zh_TW");
    if (lower == QLatin1String("en"))
        return QStringLiteral("en");
    if (lower == QLatin1String("fr"))
        return QStringLiteral("fr");
    return QString();
}

} // namespace

QStringList Localization::availableLanguages()
{
    return QStringList() << QStringLiteral("zh_CN") << QStringLiteral("zh_TW")
                         << QStringLiteral("en") << QStringLiteral("fr");
}

QString Localization::nativeName(const QString &code)
{
    if (code == QLatin1String("zh_TW"))
        return QStringLiteral("繁體中文");
    if (code == QLatin1String("en"))
        return QStringLiteral("English");
    if (code == QLatin1String("fr"))
        return QStringLiteral("Français");
    return QStringLiteral("简体中文");
}

QString Localization::systemLanguage()
{
    // 系统区域是 zh_TW / zh_HK / zh_MO 时用繁体，其余中文用简体；
    // 非中英法区域（德语、日语……）没有翻译，退回源语言简体中文
    const QString code = normalize(QLocale::system().name());
    return code.isEmpty() ? QString::fromLatin1(kSourceLanguage) : code;
}

bool Localization::install(const QString &requested)
{
    const QString normalized = normalize(requested);
    const QString effective = normalized.isEmpty() ? systemLanguage() : normalized;
    const QString previous = g_current;

    if (effective == previous
        && (effective == QLatin1String(kSourceLanguage) || g_translator != nullptr))
        return false;

    if (g_translator) {
        QCoreApplication::removeTranslator(g_translator);
        delete g_translator;
        g_translator = nullptr;
    }

    QString active = effective;
    if (effective != QLatin1String(kSourceLanguage)) {
        QTranslator *translator = new QTranslator(QCoreApplication::instance());
        if (translator->load(QStringLiteral(":/i18n/listery_%1.qm").arg(effective))) {
            QCoreApplication::installTranslator(translator);
            g_translator = translator;
        } else {
            // 语言包未随程序编译进来（例如构建时缺少 lrelease）：保持源语言，
            // 不要留下一个“声称是英文、界面却是中文”的状态
            qWarning("Listery: 找不到语言包 %s，界面回退简体中文", qPrintable(effective));
            delete translator;
            active = QString::fromLatin1(kSourceLanguage);
        }
    }

    g_current = active;
    // 数字分组、日期格式跟着界面语言走（状态栏里的“已索引 128,436 个”）
    QLocale::setDefault(QLocale(active));
    return active != previous;
}

QString Localization::current()
{
    return g_current.isEmpty() ? QString::fromLatin1(kSourceLanguage) : g_current;
}
