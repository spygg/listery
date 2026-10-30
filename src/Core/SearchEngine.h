#pragma once

#include "Core/Database.h"

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

/** 一条命中结果：记录本体 + 综合得分 + 名称中需要高亮的字符位置。 */
struct SearchResult
{
    ExecutableRecord record;
    double score = 0.0;
    QVector<int> highlights;

    // ---- 插件结果（四个字段任一非空即视为插件行，此时不走可执行文件逻辑）----
    QString pluginTitle;
    QString pluginSubtitle;
    /** 行首徽标文字，如“译”。 */
    QString pluginBadge;
    /** Enter 执行的动作，见 sdk/listery_plugin_api.h 的 action 协议。 */
    QString pluginAction;

    bool isPluginResult() const { return !pluginTitle.isEmpty() || !pluginAction.isEmpty(); }
};

Q_DECLARE_METATYPE(SearchResult)
Q_DECLARE_METATYPE(QVector<SearchResult>)

/**
 * 检索引擎：先由 SQLite 做粗筛，再在内存里按“名称匹配度 + 使用习惯”精排。
 *
 * 打分结构（0~1）：
 *   最终分 = 0.72 × 名称匹配度 + 0.28 × 使用习惯
 *   使用习惯 = 0.65 × 频次(对数归一) + 0.35 × 最近使用(半衰期 14 天)
 * 因此常用程序会自然上浮，但输入越精确时匹配度仍然主导排序。
 */
class SearchEngine
{
public:
    explicit SearchEngine(Database *database);

    QVector<SearchResult> search(const QString &query, int limit) const;

    static QStringList tokenize(const QString &query);

private:
    Database *m_database = nullptr;
    /**
     * usageSnapshot() 的短时缓存：统计表只有启动程序时才会变，
     * 连续打字（每次按键一次检索）没必要每次都把整表读进 QHash。
     * 1.5 秒足够短，刚启动过的程序下次唤起时排名就能立刻跟上。
     */
    mutable QHash<QString, UsageStat> m_usageCache;
    mutable qint64 m_usageCacheAt = 0;
};
