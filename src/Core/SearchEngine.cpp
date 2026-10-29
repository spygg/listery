#include "Core/SearchEngine.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace {
const double kMatchWeight = 0.72;
const double kUsageWeight = 0.28;
const double kRecencyHalfLifeDays = 14.0;
const int kCandidateLimit = 6000;
const int kPrefixLimit = 600;

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
};

/** 计算单个关键字与文件名的匹配质量；返回 -1 表示不匹配。 */
TokenMatch matchToken(const QString &token, const QString &name, const QString &dir)
{
    TokenMatch match;
    if (token.isEmpty())
        return match;

    if (name == token) {
        match.score = 1.55;
        for (int i = 0; i < name.size(); ++i)
            match.positions.append(i);
        return match;
    }

    if (name.startsWith(token)) {
        match.score = 1.32;
        for (int i = 0; i < token.size(); ++i)
            match.positions.append(i);
        return match;
    }

    const int index = name.indexOf(token);
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
        if (name.at(i) == token.at(tokenIndex)) {
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

    // 名称没命中，退而匹配目录：用户在输入 “program files” 之类的路径片段
    const int dirIndex = dir.indexOf(token);
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

double usageScore(const ExecutableRecord &record, int maxFrequency)
{
    if (record.launchCount <= 0)
        return 0.0;

    const double frequency = std::log1p(static_cast<double>(record.launchCount))
        / std::log1p(static_cast<double>(qMax(1, maxFrequency)));

    double recency = 0.0;
    if (record.lastUsed > 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
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

    // 粗筛：前缀命中走索引，全模糊兜底；两路结果合并去重
    QVector<ExecutableRecord> candidates;
    QHash<QString, bool> seen;
    candidates.reserve(kPrefixLimit + 256);

    if (tokens.size() == 1 && tokens.first().size() >= 2) {
        const QVector<ExecutableRecord> prefixHits = m_database->searchByPrefix(tokens.first(), kPrefixLimit);
        for (const ExecutableRecord &record : prefixHits) {
            seen.insert(record.path, true);
            candidates.append(record);
        }
    }

    const QVector<ExecutableRecord> broadHits = m_database->searchByTokens(tokens, kCandidateLimit);
    for (const ExecutableRecord &record : broadHits) {
        if (seen.contains(record.path))
            continue;
        seen.insert(record.path, true);
        candidates.append(record);
    }

    if (candidates.isEmpty())
        return results;

    // 频次归一化基准取候选集内的最大值，避免全库统计被少数超高频程序拉平
    int maxFrequency = 1;
    for (const ExecutableRecord &record : candidates)
        maxFrequency = qMax(maxFrequency, record.launchCount);

    const QString fullName = query.trimmed().toLower();
    const bool singleToken = tokens.size() == 1;

    for (const ExecutableRecord &record : candidates) {
        const QString nameLower = record.displayName.toLower();
        const QString dirLower = record.dir.toLower();

        double matchSum = 0.0;
        QVector<int> positions;
        bool matched = true;

        for (const QString &token : tokens) {
            const TokenMatch tokenMatch = matchToken(token, nameLower, dirLower);
            if (tokenMatch.score < 0.0) {
                matched = false;
                break;
            }
            matchSum += tokenMatch.score;
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
            const QString baseName = QFileInfo(record.displayName).completeBaseName().toLower();
            if (fullName == nameLower || fullName == baseName)
                matchQuality = qMin(1.0, matchQuality + 0.15);
        }

        SearchResult result;
        result.record = record;
        result.highlights = positions;
        result.score = kMatchWeight * matchQuality
            + kUsageWeight * usageScore(record, maxFrequency)
            + 0.004 * (1.0 - qMin(1.0, static_cast<double>(nameLower.size()) / 64.0));
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
