#include "Core/SearchEngine.h"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace {
const double kMatchWeight = 0.72;
const double kUsageWeight = 0.28;
const double kRecencyHalfLifeDays = 14.0;
const int kCandidateLimit = 3000;
const int kPrefixLimit = 600;
const int kSeedLimit = 64;
/**
 * 整词缩写命中（token 恰好等于某个拼音首字母变体，如 wx = 微信）的额外加成。
 * 略高于使用度权重上限 0.28：缩写是明确的输入意图，不能被一次启动记录掀翻，
 * 否则 wx 会被同名前缀的 WXWork/WXDrive 压住。
 */
const double kAbbrevExactBonus = 0.30;
/** 使用统计快照的缓存时长（毫秒）。 */
const qint64 kUsageCacheTtlMs = 1500;

bool isWordBoundary(QChar ch)
{
    return ch == QLatin1Char('-') || ch == QLatin1Char('_') || ch == QLatin1Char('.')
        || ch == QLatin1Char(' ') || ch == QLatin1Char('\\') || ch == QLatin1Char('/')
        || ch == QLatin1Char('(') || ch == QLatin1Char('[');
}

struct TokenMatch
{
    double score = -1.0;
    QVector<int> positions;
    /** token 恰好等于某个拼音首字母变体（整词缩写）。 */
    bool abbrevExact = false;
};

/** 在拼音变体串（空格分隔）里找关键字；只接受纯 ASCII 关键字，返回 -1 表示不匹配。 */
double pinyinScore(const QString &token, const QString &variants,
                   double exactScore, double prefixScore, double containsScore,
                   bool *exactHit = nullptr)
{
    if (variants.isEmpty() || token.isEmpty())
        return -1.0;
    for (const QChar ch : token) {
        if (ch.unicode() >= 128)
            return -1.0;
    }

    double best = -1.0;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QStringList list = variants.split(QLatin1Char(' '), Qt::SkipEmptyParts);
#else
    const QStringList list = variants.split(QLatin1Char(' '), QString::SkipEmptyParts);
#endif
    for (const QString &variant : list) {
        if (variant.compare(token, Qt::CaseInsensitive) == 0) {
            if (exactHit)
                *exactHit = true;
            return exactScore;
        }
        if (variant.startsWith(token, Qt::CaseInsensitive)) {
            best = qMax(best, prefixScore);
        } else if (containsScore > 0.0 && variant.contains(token, Qt::CaseInsensitive)) {
            best = qMax(best, containsScore);
        }
    }
    return best;
}

/** 计算单个关键字与文件名/拼音的匹配质量；返回 -1 表示不匹配。大小写不敏感，避免逐条 toLower 分配。 */
TokenMatch matchToken(const QString &token, const QString &name, const QString &dir,
                      const QString &pinyin, const QString &initials)
{
    TokenMatch match;
    if (token.isEmpty())
        return match;

    if (name.compare(token, Qt::CaseInsensitive) == 0) {
        match.score = 1.55;
        for (int i = 0; i < name.size(); ++i)
            match.positions.append(i);
        return match;
    }

    if (name.startsWith(token, Qt::CaseInsensitive)) {
        match.score = 1.32;
        for (int i = 0; i < token.size(); ++i)
            match.positions.append(i);
        return match;
    }

    const int index = name.indexOf(token, 0, Qt::CaseInsensitive);
    if (index >= 0) {
        double score = 1.05;
        score -= 0.25 * static_cast<double>(index) / static_cast<double>(qMax(1, name.size()));
        if (index > 0 && isWordBoundary(name.at(index - 1)))
            score += 0.12;
        match.score = score;
        for (int i = 0; i < token.size(); ++i)
            match.positions.append(index + i);
        return match;
    }

    // 子序列匹配：允许 “vscl” 命中 “visual_studio_code_launcher”
    int start = -1;
    int end = -1;
    int tokenIndex = 0;
    QVector<int> positions;
    for (int i = 0; i < name.size() && tokenIndex < token.size(); ++i) {
        if (name.at(i).toCaseFolded() == token.at(tokenIndex).toCaseFolded()) {
            if (start < 0)
                start = i;
            end = i;
            positions.append(i);
            ++tokenIndex;
        }
    }
    if (tokenIndex == token.size() && start >= 0) {
        const double density = static_cast<double>(token.size())
            / static_cast<double>(end - start + 1);
        double score = 0.45 + 0.18 * density;
        if (start == 0)
            score += 0.10;
        match.score = score;
        match.positions = positions;
        return match;
    }

    // 汉字名的拼音：全拼与首字母都算，分数略低于同等的名字命中
    const double pinyinHit = pinyinScore(token, pinyin, 1.5, 1.2, 0.9);
    if (pinyinHit > 0.0) {
        match.score = pinyinHit;
        return match;
    }
    const double initialsHit = pinyinScore(token, initials, 1.36, 1.02, -1.0, &match.abbrevExact);
    if (initialsHit > 0.0) {
        match.score = initialsHit;
        return match;
    }

    // 名称没命中，退而匹配目录：用户在输入 “program files” 之类的路径片段
    const int dirIndex = dir.indexOf(token, 0, Qt::CaseInsensitive);
    if (dirIndex >= 0) {
        double score = 0.30;
        const int lastSlash = dir.lastIndexOf(QLatin1Char('/'));
        const int altSlash = dir.lastIndexOf(QLatin1Char('\\'));
        const int lastSeparator = qMax(lastSlash, altSlash);
        if (dirIndex > lastSeparator)
            score = 0.42; // 命中最后一级目录（程序所在文件夹）
        if (dirIndex + token.size() == dir.size()
            || dir.at(dirIndex + token.size()) == QLatin1Char('/')
            || dir.at(dirIndex + token.size()) == QLatin1Char('\\'))
            score += 0.06;
        match.score = score;
        match.positions.clear();
    }
    return match;
}

/**
 * @param now 当前时间戳，由调用方取一次传进来：这个函数会对上百条候选各调一次，
 *            在循环里反复取系统时间既慢又会让同一批结果的“最近使用”基准不一致。
 */
double usageScore(const ExecutableRecord &record, int maxFrequency, qint64 now)
{
    if (record.launchCount <= 0)
        return 0.0;

    const double frequency = std::log1p(static_cast<double>(record.launchCount))
        / std::log1p(static_cast<double>(qMax(1, maxFrequency)));

    double recency = 0.0;
    if (record.lastUsed > 0) {
        const double days = qMax(0.0, static_cast<double>(now - record.lastUsed) / 86400000.0);
        recency = std::exp(-days / kRecencyHalfLifeDays);
    }
    return 0.65 * frequency + 0.35 * recency;
}

QVector<SearchResult> toResults(const QVector<ExecutableRecord> &records)
{
    QVector<SearchResult> results;
    results.reserve(records.size());
    for (const ExecutableRecord &record : records) {
        SearchResult result;
        result.record = record;
        results.append(result);
    }
    return results;
}
} // namespace

SearchEngine::SearchEngine(Database *database)
    : m_database(database)
{
}

QStringList SearchEngine::tokenize(const QString &query)
{
    QString normalized = query.trimmed().toLower();
    QStringList tokens;
    if (normalized.isEmpty())
        return tokens;

    static const QRegularExpression separator(QStringLiteral("\\s+"));
    for (const QString &part : normalized.split(separator)) {
        if (!part.isEmpty() && !tokens.contains(part))
            tokens.append(part);
    }
    return tokens;
}

QVector<SearchResult> SearchEngine::search(const QString &query, int limit) const
{
    QVector<SearchResult> results;
    if (!m_database || limit <= 0)
        return results;

    const QStringList tokens = tokenize(query);

    // 空查询：优先展示最近使用过的程序，不足时用最近改动的补齐
    if (tokens.isEmpty()) {
        QVector<ExecutableRecord> records = m_database->recentlyUsed(limit);
        if (records.size() < limit) {
            const QVector<ExecutableRecord> recent = m_database->recentlyModified(limit * 2);
            QHash<QString, bool> seen;
            for (const ExecutableRecord &record : records)
                seen.insert(record.path, true);
            for (const ExecutableRecord &record : recent) {
                if (records.size() >= limit)
                    break;
                if (seen.contains(record.path))
                    continue;
                seen.insert(record.path, true);
                records.append(record);
            }
        }
        return toResults(records);
    }

    // 粗筛：FTS5 走倒排索引做词首前缀匹配（20 万条约 5~15ms）；没有 FTS5 时退回索引前缀 + 全模糊
    QVector<ExecutableRecord> candidates;
    QHash<QString, bool> seen;
    candidates.reserve(kPrefixLimit + 256);

    const bool fts = m_database->ftsAvailable();
    const bool singleToken = tokens.size() == 1;
    // 单字符只按文件名开头匹配，避免首个按键就把整个词库都翻出来
    const bool singleChar = singleToken && tokens.first().size() == 1;

    auto appendCandidate = [&candidates, &seen](const QVector<ExecutableRecord> &records) {
        for (const ExecutableRecord &record : records) {
            if (seen.contains(record.path))
                continue;
            seen.insert(record.path, true);
            candidates.append(record);
        }
    };

    // 用过的程序先入候选：FTS 按 rowid 截断时，最常用的程序常被挤掉
    appendCandidate(m_database->searchUsageSeeds(tokens, kSeedLimit));

    // 完整文件名（含扩展名）走主键精确命中，输入 kylin.bat 这类全名时零延迟
    if (singleToken)
        appendCandidate(m_database->searchByExactName(tokens.first(), 8));

    // 单词输入先走 name 索引做前缀范围扫描（1~2ms）：逐字打字时绝大多数输入都是目标名的前缀，
    // 这一步就能填满结果，不必再碰 FTS 倒排
    QVector<ExecutableRecord> fastHits;
    if (singleToken)
        fastHits = m_database->searchByPrefix(tokens.first(), kPrefixLimit);

    // 前缀索引覆盖不到“词中出现”的情况（studio -> visual studio code），交给 FTS；
    // 前缀已经填满一屏时这一趟可以省掉
    bool wordStartHit = !fastHits.isEmpty();
    if (fts && (!singleToken || fastHits.size() < limit)) {
        const QVector<ExecutableRecord> wordHits = m_database->searchByFts(
            tokens, singleToken ? kPrefixLimit : kCandidateLimit, singleChar);
        wordStartHit = wordStartHit || !wordHits.isEmpty();
        appendCandidate(wordHits);
    }
    appendCandidate(fastHits);

    // FTS 只覆盖“词首前缀”，像 “studio” 命中 “visualstudio” 这类中缀输入要靠子串扫描补；
    // 但子串扫描是 50ms 级的全表 LIKE，只有前缀匹配完全落空时才值得付这个代价
    // —— 输入 kylin.bat 这类精确文件名时 FTS 只有 1 条命中，再去扫全表纯属浪费
    const bool needBroadScan = !fts || !wordStartHit;
    if (needBroadScan)
        appendCandidate(m_database->searchByTokens(tokens, kCandidateLimit));

    if (candidates.isEmpty())
        return results;

    // 使用统计整表很小，一次读出比每行 LEFT JOIN 便宜得多；再叠一层 1.5 秒缓存，
    // 避免逐字输入时每次按键都重新读一遍表
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_usageCacheAt == 0 || nowMs - m_usageCacheAt >= kUsageCacheTtlMs) {
        m_usageCache = m_database->usageSnapshot();
        m_usageCacheAt = nowMs;
    }
    const QHash<QString, UsageStat> &usage = m_usageCache;
    for (ExecutableRecord &record : candidates) {
        const auto it = usage.constFind(record.path);
        if (it == usage.constEnd())
            continue;
        record.launchCount = it->launchCount;
        record.lastUsed = it->lastUsed;
    }

    // 频次归一化基准取候选集内的最大值，避免全库统计被少数超高频程序拉平
    int maxFrequency = 1;
    for (const ExecutableRecord &record : candidates)
        maxFrequency = qMax(maxFrequency, record.launchCount);

    const QString fullName = query.trimmed();

    for (const ExecutableRecord &record : candidates) {
        double matchSum = 0.0;
        QVector<int> positions;
        bool matched = true;
        bool abbrevExact = false;

        for (const QString &token : tokens) {
            const TokenMatch tokenMatch = matchToken(token, record.displayName, record.dir,
                                                     record.pinyin, record.pinyinInitials);
            if (tokenMatch.score < 0.0) {
                matched = false;
                break;
            }
            matchSum += tokenMatch.score;
            abbrevExact = abbrevExact || tokenMatch.abbrevExact;
            for (int position : tokenMatch.positions) {
                if (position >= 0 && position < record.displayName.size()
                    && !positions.contains(position))
                    positions.append(position);
            }
        }
        if (!matched)
            continue;

        double matchQuality = qMin(1.0, matchSum / static_cast<double>(tokens.size()) / 1.35);

        // 完整文件名（含扩展名）或去掉扩展名后完全一致的额外加权
        if (singleToken) {
            const QString &display = record.displayName;
            bool exact = display.compare(fullName, Qt::CaseInsensitive) == 0;
            if (!exact && !record.ext.isEmpty()
                && display.size() > record.ext.size()
                && display.at(display.size() - record.ext.size() - 1) == QLatin1Char('.')) {
                exact = display.left(display.size() - record.ext.size() - 1)
                            .compare(fullName, Qt::CaseInsensitive) == 0;
            }
            if (exact)
                matchQuality = qMin(1.0, matchQuality + 0.15);
        }

        SearchResult result;
        result.record = record;
        result.highlights = positions;
        result.score = kMatchWeight * matchQuality
            + kUsageWeight * usageScore(record, maxFrequency, nowMs)
            + (abbrevExact ? kAbbrevExactBonus : 0.0)
            + 0.004 * (1.0 - qMin(1.0, static_cast<double>(record.displayName.size()) / 64.0));
        results.append(result);
    }

    std::stable_sort(results.begin(), results.end(),
                     [](const SearchResult &left, const SearchResult &right) {
                         return left.score > right.score;
                     });

    if (results.size() > limit)
        results.resize(limit);
    return results;
}
