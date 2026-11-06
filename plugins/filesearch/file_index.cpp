#include "file_index.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <queue>

namespace filesearch {
namespace {

constexpr uint32_t kSnapshotVersion = 7;
constexpr uint32_t kMaxFiles = 50000000u;
constexpr uint32_t kMaxDirs = 10000000u;
constexpr uint32_t kMaxBlob = 0x7FFFFFFFu; // 2GB - 1，避免 size_t 溢出

#pragma pack(push, 1)
struct SnapshotHeader
{
    char magic[4];
    uint32_t version;
    uint64_t builtAt;
    uint64_t scanMs;
    uint32_t fileCount;
    uint32_t dirCount;
    uint32_t namesSize;
    uint32_t dirNamesSize;
    uint32_t sigCount;
    uint32_t dirSigCount;
};
#pragma pack(pop)
static_assert(sizeof(SnapshotHeader) == 48, "SnapshotHeader must stay 48 bytes");

inline unsigned char lowerAscii(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c;
}

inline bool isAsciiSpaceChar(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string lowerAsciiCopy(const std::string &text)
{
    std::string out = text;
    for (char &c : out)
        c = static_cast<char>(lowerAscii(static_cast<unsigned char>(c)));
    return out;
}

std::string trimAscii(const std::string &text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && isAsciiSpaceChar(text[begin]))
        ++begin;
    while (end > begin && isAsciiSpaceChar(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

bool equalsNoCase(const char *text, size_t len, const std::string &needle)
{
    if (len != needle.size())
        return false;
    for (size_t i = 0; i < len; ++i) {
        if (lowerAscii(static_cast<unsigned char>(text[i]))
            != lowerAscii(static_cast<unsigned char>(needle[i])))
            return false;
    }
    return true;
}

bool startsWithNoCase(const char *text, size_t len, const std::string &needle)
{
    if (needle.size() > len)
        return false;
    for (size_t i = 0; i < needle.size(); ++i) {
        if (lowerAscii(static_cast<unsigned char>(text[i]))
            != lowerAscii(static_cast<unsigned char>(needle[i])))
            return false;
    }
    return true;
}

bool containsNoCase(const char *text, size_t len, const char *needle, size_t needleLen)
{
    if (needleLen == 0)
        return true;
    if (needleLen > len)
        return false;

    const unsigned char first = lowerAscii(static_cast<unsigned char>(needle[0]));
    const size_t last = len - needleLen;
    for (size_t i = 0; i <= last; ++i) {
        if (lowerAscii(static_cast<unsigned char>(text[i])) != first)
            continue;
        size_t j = 1;
        for (; j < needleLen; ++j) {
            if (lowerAscii(static_cast<unsigned char>(text[i + j]))
                != lowerAscii(static_cast<unsigned char>(needle[j])))
                break;
        }
        if (j == needleLen)
            return true;
    }
    return false;
}

/** 路径比较：忽略大小写，并且 '/' 与 '\' 视为同一个分隔符。 */
inline unsigned char normalizePathChar(unsigned char c)
{
    return c == '/' ? '\\' : lowerAscii(c);
}

bool containsNoCasePath(const char *text, size_t len, const char *needle, size_t needleLen)
{
    if (needleLen == 0)
        return true;
    if (needleLen > len)
        return false;

    const unsigned char first = normalizePathChar(static_cast<unsigned char>(needle[0]));
    const size_t last = len - needleLen;
    for (size_t i = 0; i <= last; ++i) {
        if (normalizePathChar(static_cast<unsigned char>(text[i])) != first)
            continue;
        size_t j = 1;
        for (; j < needleLen; ++j) {
            if (normalizePathChar(static_cast<unsigned char>(text[i + j]))
                != normalizePathChar(static_cast<unsigned char>(needle[j])))
                break;
        }
        if (j == needleLen)
            return true;
    }
    return false;
}

inline bool hasPathWildcard(const std::string &token)
{
    return token.find('*') != std::string::npos || token.find('?') != std::string::npos;
}

/** 从 start 开始用通配符匹配 text 的一个前缀；模式消费完即成功。 */
bool globPrefixMatchNoCasePath(const char *text, size_t len, const char *pattern, size_t patternLen)
{
    size_t ti = 0;
    size_t pi = 0;
    size_t starPattern = std::string::npos;
    size_t starText = 0;

    while (pi < patternLen) {
        const unsigned char current = static_cast<unsigned char>(pattern[pi]);
        if (current == '*') {
            starPattern = pi++;
            starText = ti;
            continue;
        }
        if (ti < len
            && (current == '?'
                || normalizePathChar(current)
                       == normalizePathChar(static_cast<unsigned char>(text[ti])))) {
            ++ti;
            ++pi;
            continue;
        }
        if (starPattern != std::string::npos) {
            if (starText >= len)
                return false;
            pi = starPattern + 1;
            ++starText;
            ti = starText;
            continue;
        }
        return false;
    }
    return true;
}

/**
 * 通配符路径匹配：* 匹配任意长度（可以跨目录），? 匹配一个字符。
 * 语义和 containsNoCasePath 一样是“包含”：模式可以从路径任意位置开始。
 */
bool globContainsNoCasePath(const char *text, size_t len, const char *pattern, size_t patternLen)
{
    if (patternLen == 0)
        return true;
    if (pattern[0] == '*')
        return globPrefixMatchNoCasePath(text, len, pattern, patternLen);

    const unsigned char first = static_cast<unsigned char>(pattern[0]);
    const bool anyFirst = first == '?';
    for (size_t start = 0; start < len; ++start) {
        if (!anyFirst
            && normalizePathChar(first)
                   != normalizePathChar(static_cast<unsigned char>(text[start])))
            continue;
        if (globPrefixMatchNoCasePath(text + start, len - start, pattern, patternLen))
            return true;
    }
    return false;
}

/** 路径片段是否命中：含 * 或 ? 时按通配符匹配，否则按子串匹配。 */
bool pathTextMatches(const char *text, size_t len, const char *token, size_t tokenLen)
{
    if (std::memchr(token, '*', tokenLen) != nullptr
        || std::memchr(token, '?', tokenLen) != nullptr)
        return globContainsNoCasePath(text, len, token, tokenLen);
    return containsNoCasePath(text, len, token, tokenLen);
}

bool pathTokenMatches(const char *text, size_t len, const std::string &token)
{
    return pathTextMatches(text, len, token.data(), token.size());
}

/**
 * 取出模式里最长的一段纯字面量，用来判断“命中在文件名里的位置”。
 * 通配符 * ? 会把字面量切成多段；返回最长的一段（找不到时返回空串）。
 */
std::string longestLiteralRun(const std::string &pattern)
{
    std::string best;
    std::string current;
    auto flush = [&best, &current]() {
        if (current.size() > best.size())
            best = current;
        current.clear();
    };
    for (char ch : pattern) {
        if (ch == '*' || ch == '?') {
            flush();
            continue;
        }
        current.push_back(ch);
    }
    flush();
    return best;
}

/** needle 在 text 里第一次出现的位置；找不到返回 len（排序时排最后）。 */
uint32_t literalPositionNoCase(const char *text, size_t len, const std::string &needle)
{
    if (needle.empty())
        return 0;
    if (needle.size() > len)
        return static_cast<uint32_t>(len);
    const size_t last = len - needle.size();
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (lowerAscii(static_cast<unsigned char>(text[i + j]))
                != lowerAscii(static_cast<unsigned char>(needle[j])))
                break;
        }
        if (j == needle.size())
            return static_cast<uint32_t>(i);
    }
    return static_cast<uint32_t>(len);
}

inline bool isAsciiAlnum(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/**
 * 从正则里挑出最长的一段“必须出现”的字面量，用来做签名预筛。
 * 字符类、量词和 \d \w \s 之类的转义不算字面量；\. 这种转义标点按标点本身算。
 * 只要出现分组或或运算就放弃预筛：那会改变“必须出现”的语义，宁可慢也不能漏。
 * 返回小写的字面量；长度 < 2 时返回空串（没有可用的预筛关键字）。
 */
std::string regexLiteralKey(const std::string &pattern)
{
    // 先扫一遍：括号和 | 意味着可选分组或分支，字面量不再保证出现
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch == '\\') {
            ++i;
            continue;
        }
        if (ch == '[') {
            size_t j = i + 1;
            if (j < pattern.size() && pattern[j] == '^')
                ++j;
            if (j < pattern.size() && pattern[j] == ']')
                ++j;
            while (j < pattern.size() && pattern[j] != ']') {
                if (pattern[j] == '\\')
                    ++j;
                ++j;
            }
            i = j < pattern.size() ? j : pattern.size();
            continue;
        }
        if (ch == '(' || ch == '|')
            return std::string();
    }

    std::string best;
    std::string current;
    auto flush = [&best, &current]() {
        if (current.size() > best.size())
            best = current;
        current.clear();
    };

    for (size_t i = 0; i < pattern.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(pattern[i]);
        if (ch == '\\') {
            if (i + 1 >= pattern.size()) {
                flush();
                break;
            }
            const unsigned char next = static_cast<unsigned char>(pattern[i + 1]);
            if (isAsciiAlnum(next)) {
                flush(); // \d \w \s \b 等：字符类或断言，不是固定的字面量
                // \x41、\1 之类的转义把后面的字符也吃掉了，跳过它们
                size_t digits = 0;
                if (next == 'x')
                    digits = 2;
                else if (next == 'u')
                    digits = 4;
                else if (next == 'U')
                    digits = 8;
                else if (next >= '0' && next <= '9')
                    digits = pattern.size();
                while (digits > 0 && i + 2 < pattern.size() && isAsciiAlnum(
                           static_cast<unsigned char>(pattern[i + 2]))) {
                    ++i;
                    --digits;
                }
            } else {
                current.push_back(static_cast<char>(lowerAscii(next)));
            }
            ++i;
            continue;
        }
        if (ch == '[') {
            flush();
            size_t j = i + 1;
            if (j < pattern.size() && pattern[j] == '^')
                ++j;
            if (j < pattern.size() && pattern[j] == ']')
                ++j;
            while (j < pattern.size() && pattern[j] != ']') {
                if (pattern[j] == '\\')
                    ++j;
                ++j;
            }
            i = j < pattern.size() ? j : pattern.size();
            continue;
        }
        if (ch == '{') {
            // {m,n} 量词里的数字不是字面量；m == 0 时前一个字符可能不出现
            size_t j = i + 1;
            bool hasMin = false;
            unsigned minValue = 0;
            while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') {
                hasMin = true;
                minValue = minValue * 10 + static_cast<unsigned>(pattern[j] - '0');
                if (minValue > 1000)
                    minValue = 1000;
                ++j;
            }
            if ((!hasMin || minValue == 0) && !current.empty())
                current.pop_back();
            flush();
            while (i + 1 < pattern.size() && pattern[i + 1] != '}')
                ++i;
            if (i + 1 < pattern.size())
                ++i;
            continue;
        }
        if (ch == '*' || ch == '?') {
            // 0 次也可能匹配：前一个字符不是必须出现，保守起见从关键字里去掉
            if (!current.empty())
                current.pop_back();
            flush();
            continue;
        }
        if (ch == '+') {
            // 至少 1 次，前一个字符仍然必须出现，整段保留
            flush();
            continue;
        }
        if (ch == '.' || ch == '^' || ch == '$' || ch == ')' || ch == '}') {
            flush();
            continue;
        }
        current.push_back(static_cast<char>(lowerAscii(ch)));
    }
    flush();

    return best.size() >= 2 ? best : std::string();
}

/**
 * 64 位 bigram 签名。子串的所有 bigram 一定出现在父串的签名里，
 * 因此可以用 (nameSig & queryMask) == queryMask 先过滤掉绝大多数候选。
 * 同时加入单字符的 bit：像 "zzzzz" 这种只有一个 bigram 的关键字也能少很多误报。
 * 只有 1 个字符的关键字不生成 bigram bit（仍然有单字符 bit）。
 */
uint64_t nameSignature(const char *text, size_t len)
{
    if (len == 0)
        return 0;
    uint64_t signature = 0;

    const unsigned char first = lowerAscii(static_cast<unsigned char>(text[0]));
    uint32_t firstHash = static_cast<uint32_t>(first) * 0x9E3779B1u;
    firstHash ^= firstHash >> 13;
    signature |= 1ull << (firstHash & 63);

    if (len < 2)
        return signature;

    unsigned char previous = first;
    for (size_t i = 1; i < len; ++i) {
        const unsigned char current = lowerAscii(static_cast<unsigned char>(text[i]));
        uint32_t charHash = static_cast<uint32_t>(current) * 0x9E3779B1u;
        charHash ^= charHash >> 13;
        signature |= 1ull << (charHash & 63);

        uint32_t key = static_cast<uint32_t>(previous)
                       | (static_cast<uint32_t>(current) << 8);
        uint32_t hash = key * 0x9E3779B1u;
        hash ^= hash >> 13;
        signature |= 1ull << (hash & 63);
        previous = current;
    }
    return signature;
}

uint64_t querySignature(const std::vector<std::string> &tokens)
{
    uint64_t mask = 0;
    for (const std::string &token : tokens)
        mask |= nameSignature(token.data(), token.size());
    return mask;
}

void addSignature256Bit(Signature256 &signature, uint32_t hash)
{
    const uint32_t bit = hash & 255;
    signature.words[bit >> 6] |= 1ull << (bit & 63);
}

Signature256 nameSignature256(const char *text, size_t len)
{
    Signature256 signature;
    if (len == 0)
        return signature;

    const unsigned char first = lowerAscii(static_cast<unsigned char>(text[0]));
    uint32_t firstHash = static_cast<uint32_t>(first) * 0x9E3779B1u;
    firstHash ^= firstHash >> 13;
    addSignature256Bit(signature, firstHash);

    unsigned char previous = first;
    for (size_t i = 1; i < len; ++i) {
        const unsigned char current = lowerAscii(static_cast<unsigned char>(text[i]));
        uint32_t charHash = static_cast<uint32_t>(current) * 0x9E3779B1u;
        charHash ^= charHash >> 13;
        addSignature256Bit(signature, charHash);

        uint32_t key = static_cast<uint32_t>(previous)
                       | (static_cast<uint32_t>(current) << 8);
        uint32_t hash = key * 0x9E3779B1u;
        hash ^= hash >> 13;
        addSignature256Bit(signature, hash);
        previous = current;
    }
    return signature;
}

Signature256 querySignature256(const std::vector<std::string> &tokens)
{
    Signature256 mask;
    for (const std::string &token : tokens) {
        if (!hasPathWildcard(token))
            mask.merge(nameSignature256(token.data(), token.size()));
    }
    return mask;
}

unsigned popcount64(uint64_t value)
{
    unsigned count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

bool splitExtension(const char *name, size_t nameLen, const char **ext, size_t *extLen)
{
    size_t dot = nameLen;
    for (size_t i = nameLen; i > 0; --i) {
        if (name[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }
    if (dot == nameLen || dot == 0 || dot + 1 >= nameLen)
        return false;
    *ext = name + dot + 1;
    *extLen = nameLen - dot - 1;
    return true;
}

uint8_t hashExtension(const char *ext, size_t extLen)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < extLen; ++i) {
        hash ^= lowerAscii(static_cast<unsigned char>(ext[i]));
        hash *= 16777619u;
    }
    // FNV-1a 的高字节对短字符串区分度很差（pdf/template/mf 会撞在一起），
    // 这里做一次 avalanche 再取低字节，保证 255 个桶分布均匀。
    hash ^= hash >> 16;
    hash *= 0x7FEB352Du;
    hash ^= hash >> 15;
    const uint8_t value = static_cast<uint8_t>(hash & 0xFF);
    return value == 0 ? 1 : value;
}

uint8_t extensionHash(const char *name, size_t nameLen)
{
    const char *ext = nullptr;
    size_t extLen = 0;
    if (!splitExtension(name, nameLen, &ext, &extLen))
        return 0;
    return hashExtension(ext, extLen);
}

uint8_t extensionTokenHash(const std::string &token)
{
    return hashExtension(token.data(), token.size());
}

void computeSignatureUnions(Index &index)
{
    uint64_t nameUnion = 0;
    for (uint64_t signature : index.sig)
        nameUnion |= signature;
    index.nameSigUnion = nameUnion;

    Signature256 dirUnion;
    for (const Signature256 &signature : index.dirSigs)
        dirUnion.merge(signature);
    index.dirSigUnion = dirUnion;
}

bool signaturePossible(uint64_t unionMask, const std::string &token)
{
    const uint64_t mask = nameSignature(token.data(), token.size());
    return mask == 0 || (unionMask & mask) == mask;
}

bool signaturePossible(const Signature256 &unionMask, const std::string &token)
{
    const Signature256 mask = nameSignature256(token.data(), token.size());
    return mask.empty() || unionMask.contains(mask);
}

int compareNameToNeedle(const std::string &name, const std::string &needle)
{
    const size_t common = (std::min)(name.size(), needle.size());
    for (size_t i = 0; i < common; ++i) {
        const unsigned char a = lowerAscii(static_cast<unsigned char>(name[i]));
        const unsigned char b = lowerAscii(static_cast<unsigned char>(needle[i]));
        if (a != b)
            return a < b ? -1 : 1;
    }
    if (name.size() == needle.size())
        return 0;
    return name.size() < needle.size() ? -1 : 1;
}

/** 在 names 上前缀压缩记录之间顺序前进。 */
class NameCursor
{
public:
    explicit NameCursor(const Index &index) : m_index(index) {}

    void seekBefore(uint32_t target)
    {
        const uint32_t blockStart = target - (target % kNameBlock);
        m_pos = blockStart;
        m_off = blockStart < m_index.files.size()
                    ? static_cast<size_t>(m_index.files[blockStart].nameOff)
                    : 0;
        m_name.clear();
        while (m_pos < target)
            next();
    }

    const std::string &next()
    {
        if (m_off >= m_index.names.size()) {
            m_name.clear();
            return m_name;
        }
        const size_t record = m_off;
        const unsigned prefix = static_cast<unsigned char>(m_index.names[record]);
        const char *suffix = m_index.names.data() + record + 1;
        const size_t suffixLen = std::strlen(suffix);
        m_name.resize(prefix);
        m_name.append(suffix, suffixLen);
        m_off = record + 1 + suffixLen + 1;
        ++m_pos;
        return m_name;
    }

private:
    const Index &m_index;
    uint32_t m_pos = 0;
    size_t m_off = 0;
    std::string m_name;
};

size_t lowerBoundName(const Index &index, const std::string &needle)
{
    NameCursor cursor(index);
    size_t lo = 0;
    size_t hi = index.files.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        cursor.seekBefore(static_cast<uint32_t>(mid));
        const std::string &name = cursor.next();
        if (compareNameToNeedle(name, needle) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/** 解码单个文件名，复用调用方缓冲区，避免每个候选都分配字符串。 */
bool decodeFileNameInto(const Index &index, uint32_t fileIndex, std::string &out)
{
    if (fileIndex >= index.files.size())
        return false;
    const uint32_t blockStart = fileIndex - (fileIndex % kNameBlock);
    size_t offset = static_cast<size_t>(index.files[blockStart].nameOff);
    out.clear();
    for (uint32_t i = blockStart; i <= fileIndex; ++i) {
        if (offset >= index.names.size())
            return false;
        const unsigned prefix = static_cast<unsigned char>(index.names[offset]);
        const char *suffix = index.names.data() + offset + 1;
        const size_t suffixLen = std::strlen(suffix);
        out.resize(prefix);
        out.append(suffix, suffixLen);
        offset += 1 + suffixLen + 1;
    }
    return true;
}

bool fileExtensionMatches(const char *name, size_t nameLen,
                          const std::vector<std::string> &exts)
{
    if (exts.empty())
        return true;

    const char *ext = nullptr;
    size_t extLen = 0;
    if (!splitExtension(name, nameLen, &ext, &extLen))
        return false;
    for (const std::string &candidate : exts) {
        if (equalsNoCase(ext, extLen, candidate))
            return true;
    }
    return false;
}

bool buildDirPathInto(const Index &index, uint32_t dirId, std::string &out,
                      std::vector<uint32_t> &stack)
{
    out.clear();
    stack.clear();
    if (dirId >= index.dirs.size())
        return false;

    uint32_t current = dirId;
    for (;;) {
        stack.push_back(current);
        if (stack.size() > 4096)
            return false;
        const uint32_t parent = index.dirs[current].parent;
        if (parent == kInvalidIndex)
            break;
        if (parent >= index.dirs.size())
            return false;
        current = parent;
    }

    for (size_t i = stack.size(); i > 0; --i) {
        const DirEntry &dir = index.dirs[stack[i - 1]];
        const char *name = index.dirNames.data() + dir.nameOff;
        if (!out.empty() && out.back() != '\\' && out.back() != '/')
            out.push_back('\\');
        out.append(name, dir.nameLen);
    }
    return true;
}

bool matchesFilters(const Index &index, const FileEntry &file, const char *name, size_t nameLen,
                    const ParsedQuery &query, bool allowPathForPlain, std::string &dirBuffer,
                    std::string &fullPathBuffer, std::vector<uint32_t> &ancestorBuffer,
                    uint32_t &dirCacheId)
{
    if (!fileExtensionMatches(name, nameLen, query.exts))
        return false;

    const bool needDir = !query.path.empty() || !query.fullPath.empty()
                         || (allowPathForPlain && !query.plain.empty());
    const char *dir = "";
    size_t dirLen = 0;
    if (needDir) {
        // 同一目录的候选通常连续出现，缓存上一次拼好的路径
        if (dirCacheId != file.dirId) {
            if (buildDirPathInto(index, file.dirId, dirBuffer, ancestorBuffer))
                dirCacheId = file.dirId;
            else {
                dirBuffer.clear();
                dirCacheId = kInvalidIndex;
            }
        }
        dir = dirBuffer.data();
        dirLen = dirBuffer.size();
    }

    for (const std::string &token : query.path) {
        if (!pathTokenMatches(dir, dirLen, token))
            return false;
    }

    // full: 关键字可能整个落在目录部分，也可能跨过目录和文件名的分隔符。
    if (!query.fullPath.empty()) {
        bool spansName = false;
        for (const std::string &token : query.fullPath) {
            if (!pathTokenMatches(dir, dirLen, token)) {
                spansName = true;
                break;
            }
        }
        if (spansName) {
            fullPathBuffer.assign(dir, dirLen);
            if (!fullPathBuffer.empty() && fullPathBuffer.back() != '\\'
                && fullPathBuffer.back() != '/')
                fullPathBuffer.push_back('\\');
            fullPathBuffer.append(name, nameLen);
            for (const std::string &token : query.fullPath) {
                if (!pathTokenMatches(fullPathBuffer.data(), fullPathBuffer.size(), token))
                    return false;
            }
        }
    }

    if (!query.plain.empty()) {
        for (const std::string &token : query.plain) {
            if (containsNoCase(name, nameLen, token.data(), token.size()))
                continue;
            if (allowPathForPlain && pathTokenMatches(dir, dirLen, token))
                continue;
            return false;
        }
    }

    for (const std::regex &pattern : query.regex) {
        if (!std::regex_search(name, name + nameLen, pattern))
            return false;
    }
    return true;
}

std::wstring errnoText(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                         | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0,
                                     nullptr);
    std::wstring text;
    if (len > 0 && buffer != nullptr)
        text.assign(buffer, len);
    if (buffer != nullptr)
        LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
        text.pop_back();
    return text;
}

void setError(std::wstring *error, const std::wstring &message)
{
    if (error)
        *error = message;
}

bool readExact(HANDLE file, void *data, size_t bytes)
{
    if (bytes == 0)
        return true;
    auto *cursor = static_cast<unsigned char *>(data);
    size_t remaining = bytes;
    while (remaining > 0) {
        const DWORD request = static_cast<DWORD>((std::min)(remaining, size_t(0x40000000)));
        DWORD read = 0;
        if (!ReadFile(file, cursor, request, &read, nullptr) || read == 0)
            return false;
        cursor += read;
        remaining -= read;
    }
    return true;
}

bool writeExact(HANDLE file, const void *data, size_t bytes)
{
    if (bytes == 0)
        return true;
    const auto *cursor = static_cast<const unsigned char *>(data);
    size_t remaining = bytes;
    while (remaining > 0) {
        const DWORD request = static_cast<DWORD>((std::min)(remaining, size_t(0x40000000)));
        DWORD written = 0;
        if (!WriteFile(file, cursor, request, &written, nullptr) || written == 0)
            return false;
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool validIndex(const Index &index)
{
    if (index.files.size() > kMaxFiles || index.dirs.size() > kMaxDirs)
        return false;
    if (index.sig.size() != index.files.size())
        return false;
    if (index.dirSigs.size() != index.dirs.size())
        return false;
    if (index.names.size() > kMaxBlob || index.dirNames.size() > kMaxBlob)
        return false;

    for (const DirEntry &dir : index.dirs) {
        if (static_cast<size_t>(dir.nameOff) + dir.nameLen > index.dirNames.size())
            return false;
        if (dir.parent != kInvalidIndex && dir.parent >= index.dirs.size())
            return false;
    }

    // 顺序验证前缀压缩记录：偏移必须连续，块首必须是完整名字。
    size_t offset = 0;
    uint32_t previousLen = 0;
    for (size_t i = 0; i < index.files.size(); ++i) {
        const FileEntry &file = index.files[i];
        if (file.dirId >= index.dirs.size())
            return false;
        if (file.nameOff != offset || offset >= index.names.size())
            return false;

        const unsigned prefix = static_cast<unsigned char>(index.names[offset]);
        if ((i % kNameBlock) == 0) {
            if (prefix != 0)
                return false;
        } else if (prefix > previousLen) {
            return false;
        }

        const char *suffix = index.names.data() + offset + 1;
        const size_t remaining = index.names.size() - offset - 1;
        const void *nul = std::memchr(suffix, 0, remaining);
        if (nul == nullptr)
            return false;
        const size_t suffixLen = static_cast<const char *>(nul) - suffix;
        const size_t nameLen = prefix + suffixLen;
        previousLen = static_cast<uint32_t>((std::min)(nameLen, size_t(65535)));
        offset += 1 + suffixLen + 1;
    }
    return offset == index.names.size();
}

} // namespace

uint64_t textSignature(const char *text, size_t len)
{
    return nameSignature(text, len);
}

Signature256 textSignature256(const char *text, size_t len)
{
    return nameSignature256(text, len);
}

std::string wideToUtf8(const std::wstring &text)
{
    if (text.empty())
        return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (needed <= 0)
        return std::string();
    std::string out;
    out.resize(static_cast<size_t>(needed));
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], needed,
                        nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string &text)
{
    if (text.empty())
        return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (needed <= 0)
        return std::wstring();
    std::wstring out;
    out.resize(static_cast<size_t>(needed));
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], needed);
    return out;
}

std::string formatSize(uint32_t bytes)
{
    if (bytes == kSizeCap)
        return "≥16 MB";
    char buffer[64];
    if (bytes < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%u B", bytes);
    } else if (bytes < 1024u * 1024u) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024u * 1024u * 1024u) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return std::string(buffer);
}

bool fileNameAt(const Index &index, uint32_t fileIndex, std::string &out)
{
    return decodeFileNameInto(index, fileIndex, out);
}

bool buildDirPath(const Index &index, uint32_t dirId, std::string &out)
{
    std::vector<uint32_t> stack;
    return buildDirPathInto(index, dirId, out, stack);
}

void finalizeIndex(Index &index,
                   std::vector<FileEntry> rawFiles,
                   std::vector<DirEntry> dirs,
                   std::vector<char> rawNames,
                   std::vector<char> dirNames,
                   std::vector<Signature256> dirSigs)
{
    index = Index();
    index.dirs = std::move(dirs);
    index.dirNames = std::move(dirNames);
    index.dirSigs = std::move(dirSigs);
    if (index.dirSigs.size() != index.dirs.size())
        index.dirSigs.resize(index.dirs.size());

    const size_t count = rawFiles.size();

    // 直接按文件名排序。files 之后就是检索用的顺序数组，不再保留第二份索引。
    std::sort(rawFiles.begin(), rawFiles.end(),
              [&rawNames](const FileEntry &left, const FileEntry &right) {
                  const char *a = rawNames.data() + left.nameOff;
                  const char *b = rawNames.data() + right.nameOff;
                  const size_t aLen = left.meta == 255
                                          ? std::strlen(a)
                                          : static_cast<size_t>(left.meta);
                  const size_t bLen = right.meta == 255
                                          ? std::strlen(b)
                                          : static_cast<size_t>(right.meta);
                  const size_t common = (std::min)(aLen, bLen);
                  for (size_t i = 0; i < common; ++i) {
                      const unsigned char ac = lowerAscii(static_cast<unsigned char>(a[i]));
                      const unsigned char bc = lowerAscii(static_cast<unsigned char>(b[i]));
                      if (ac != bc)
                          return ac < bc;
                  }
                  return aLen < bLen;
              });

    index.files = std::move(rawFiles);
    index.names.reserve(rawNames.size() / 2 + count * 2 + 1024);
    index.sig.resize(count);

    std::string previous;
    previous.reserve(256);
    for (size_t i = 0; i < count; ++i) {
        FileEntry &file = index.files[i];
        const char *name = rawNames.data() + file.nameOff;
        const size_t nameLen = file.meta == 255
                                   ? std::strlen(name)
                                   : static_cast<size_t>(file.meta);

        unsigned prefix = 0;
        if ((i % kNameBlock) != 0) {
            const size_t common = (std::min)(previous.size(), nameLen);
            while (prefix < common
                   && previous[prefix] == name[prefix])
                ++prefix;
            if (prefix > 255)
                prefix = 255;
        }

        file.nameOff = static_cast<uint32_t>(index.names.size());
        file.meta = extensionHash(name, nameLen);
        index.names.push_back(static_cast<char>(prefix));
        index.names.insert(index.names.end(), name + prefix, name + nameLen);
        index.names.push_back('\0');

        index.sig[i] = nameSignature(name, nameLen);
        previous.assign(name, nameLen);
    }

    computeSignatureUnions(index);

    // 原始名字不再需要，尽早在返回前释放可以降低重建索引时的峰值内存。
    std::vector<char>().swap(rawNames);
}

bool loadIndex(const std::wstring &path, Index &out, std::wstring *error)
{
    out = Index();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
            setError(error, L"打开索引失败：" + errnoText(code));
        return false;
    }

    SnapshotHeader header{};
    bool ok = readExact(file, &header, sizeof(header));
    if (!ok || std::memcmp(header.magic, "LSFS", 4) != 0 || header.version != kSnapshotVersion) {
        CloseHandle(file);
        setError(error, L"索引文件格式不匹配");
        return false;
    }
    if (header.fileCount > kMaxFiles || header.dirCount > kMaxDirs
        || header.sigCount != header.fileCount || header.dirSigCount != header.dirCount
        || header.namesSize > kMaxBlob || header.dirNamesSize > kMaxBlob) {
        CloseHandle(file);
        setError(error, L"索引文件尺寸异常");
        return false;
    }

    out.builtAt = header.builtAt;
    out.scanMs = header.scanMs;
    out.files.resize(header.fileCount);
    out.dirs.resize(header.dirCount);
    out.names.resize(header.namesSize);
    out.dirNames.resize(header.dirNamesSize);
    out.sig.resize(header.sigCount);
    out.dirSigs.resize(header.dirSigCount);

    ok = readExact(file, out.files.data(), out.files.size() * sizeof(FileEntry))
        && readExact(file, out.dirs.data(), out.dirs.size() * sizeof(DirEntry))
        && readExact(file, out.names.data(), out.names.size())
        && readExact(file, out.dirNames.data(), out.dirNames.size())
        && readExact(file, out.sig.data(), out.sig.size() * sizeof(uint64_t))
        && readExact(file, out.dirSigs.data(), out.dirSigs.size() * sizeof(Signature256));
    CloseHandle(file);

    if (!ok) {
        out = Index();
        setError(error, L"读取索引文件失败");
        return false;
    }
    if (!validIndex(out)) {
        out = Index();
        setError(error, L"索引文件内容损坏");
        return false;
    }
    computeSignatureUnions(out);
    return true;
}

bool saveIndex(const std::wstring &path, const Index &index, std::wstring *error)
{
    if (!validIndex(index)) {
        setError(error, L"拒绝保存内容异常的索引");
        return false;
    }

    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        setError(error, L"创建索引文件失败：" + errnoText(GetLastError()));
        return false;
    }

    SnapshotHeader header{};
    std::memcpy(header.magic, "LSFS", 4);
    header.version = kSnapshotVersion;
    header.builtAt = index.builtAt;
    header.scanMs = index.scanMs;
    header.fileCount = static_cast<uint32_t>(index.files.size());
    header.dirCount = static_cast<uint32_t>(index.dirs.size());
    header.namesSize = static_cast<uint32_t>(index.names.size());
    header.dirNamesSize = static_cast<uint32_t>(index.dirNames.size());
    header.sigCount = static_cast<uint32_t>(index.sig.size());
    header.dirSigCount = static_cast<uint32_t>(index.dirSigs.size());

    bool ok = writeExact(file, &header, sizeof(header))
        && writeExact(file, index.files.data(), index.files.size() * sizeof(FileEntry))
        && writeExact(file, index.dirs.data(), index.dirs.size() * sizeof(DirEntry))
        && writeExact(file, index.names.data(), index.names.size())
        && writeExact(file, index.dirNames.data(), index.dirNames.size())
        && writeExact(file, index.sig.data(), index.sig.size() * sizeof(uint64_t))
        && writeExact(file, index.dirSigs.data(), index.dirSigs.size() * sizeof(Signature256));
    CloseHandle(file);

    if (!ok) {
        DeleteFileW(temp.c_str());
        setError(error, L"写入索引文件失败");
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(temp.c_str());
        setError(error, L"替换索引文件失败：" + errnoText(code));
        return false;
    }
    return true;
}

ParsedQuery parseQuery(const std::string &utf8Query)
{
    ParsedQuery query;
    const std::string lowered = lowerAsciiCopy(utf8Query);

    size_t pos = 0;
    while (pos < lowered.size()) {
        while (pos < lowered.size() && isAsciiSpaceChar(lowered[pos]))
            ++pos;
        if (pos >= lowered.size())
            break;
        size_t end = pos;
        while (end < lowered.size() && !isAsciiSpaceChar(lowered[end]))
            ++end;
        const std::string token = lowered.substr(pos, end - pos);
        // 正则要保留原始大小写：\D 与 \d 语义不同，lowered 与原文逐字节等长，可直接切片
        const std::string original = utf8Query.substr(pos, end - pos);
        pos = end;

        if (token.empty() || token == "*")
            continue;
        if (token.rfind("re:", 0) == 0) {
            const std::string pattern = original.substr(3);
            if (!pattern.empty()) {
                try {
                    query.regex.emplace_back(pattern, std::regex::ECMAScript | std::regex::icase);
                    query.regexKeys.push_back(regexLiteralKey(pattern));
                } catch (const std::regex_error &error) {
                    if (query.regexError.empty()) {
                        query.regexError = "正则表达式无效：" + pattern;
                        const char *reason = error.what();
                        if (reason != nullptr && *reason != '\0') {
                            query.regexError += "（";
                            query.regexError += reason;
                            query.regexError += "）";
                        }
                    }
                }
            }
            continue;
        }
        if (token.rfind("ext:", 0) == 0) {
            std::string values = token.substr(4);
            size_t begin = 0;
            while (begin <= values.size()) {
                const size_t comma = values.find(',', begin);
                std::string value = trimAscii(values.substr(
                    begin, comma == std::string::npos ? std::string::npos : comma - begin));
                if (!value.empty()) {
                    if (value.front() == '.')
                        value.erase(value.begin());
                    if (!value.empty())
                        query.exts.push_back(value);
                }
                if (comma == std::string::npos)
                    break;
                begin = comma + 1;
            }
            continue;
        }
        if (token.rfind("path:", 0) == 0) {
            const std::string value = trimAscii(token.substr(5));
            if (!value.empty())
                query.path.push_back(value);
            continue;
        }
        if (token.rfind("full:", 0) == 0) {
            const std::string value = trimAscii(token.substr(5));
            if (!value.empty())
                query.fullPath.push_back(value);
            continue;
        }
        if (token.size() > 2 && token.rfind("*.", 0) == 0
            && token.find_first_of("*?", 1) == std::string::npos) {
            query.exts.push_back(token.substr(2));
            continue;
        }
        if (token.find('\\') != std::string::npos || token.find('/') != std::string::npos) {
            // 带分隔符的裸词按完整路径匹配：粘贴 C:\Users\me\report.docx 也能直接命中
            query.fullPath.push_back(token);
            continue;
        }
        if (std::find(query.plain.begin(), query.plain.end(), token) == query.plain.end())
            query.plain.push_back(token);
    }
    return query;
}

size_t utf8CodePointCount(const std::string &text)
{
    size_t count = 0;
    for (unsigned char ch : text) {
        if ((ch & 0xC0) != 0x80)
            ++count;
    }
    return count;
}

std::vector<uint32_t> searchIndex(const Index &index, const ParsedQuery &query,
                                  int maxResults, bool matchPath,
                                  const std::function<bool()> &cancelled)
{
    std::vector<uint32_t> results;
    if (maxResults <= 0 || index.files.empty() || query.empty() || !query.regexError.empty())
        return results;

    auto isCancelled = [&cancelled]() { return cancelled && cancelled(); };

    // 允许一次取回大量命中（宿主要求全部显示）；上限防止极宽查询撑爆内存。
    const size_t limit = static_cast<size_t>((std::min)(maxResults, 20000));
    results.reserve(limit);

    auto alreadySelected = [&results](uint32_t fileIdx) {
        return std::find(results.begin(), results.end(), fileIdx) != results.end();
    };
    auto add = [&results, &alreadySelected, limit](uint32_t fileIdx) {
        if (results.size() >= limit || alreadySelected(fileIdx))
            return;
        results.push_back(fileIdx);
    };
    auto full = [&results, limit]() { return results.size() >= limit; };

    const uint32_t count = static_cast<uint32_t>(index.files.size());
    // 按码点而不是 UTF-8 字节数判断：一个汉字是 3 字节，否则首键就会扫全库
    const bool singleChar =
        query.plain.size() == 1 && utf8CodePointCount(query.plain.front()) == 1;
    // 通配符里的 * ? 不能参与签名预筛，querySignature256 会自动跳过这些片段
    const Signature256 pathMask = querySignature256(query.path);
    std::vector<uint64_t> plainMasks;
    plainMasks.reserve(query.plain.size());
    for (const std::string &token : query.plain)
        plainMasks.push_back(nameSignature(token.data(), token.size()));
    std::vector<Signature256> plainDirMasks;
    plainDirMasks.reserve(query.plain.size());
    for (const std::string &token : query.plain)
        plainDirMasks.push_back(nameSignature256(token.data(), token.size()));
    // 正则没法直接做签名预筛，但正则里固定的字面量可以：先挡住绝大多数不可能命中的文件。
    std::vector<uint64_t> regexMasks;
    regexMasks.reserve(query.regexKeys.size());
    for (const std::string &key : query.regexKeys)
        regexMasks.push_back(nameSignature(key.data(), key.size()));
    // 粗筛：签名并集都不包含某个关键字的 bigram，说明全库不可能命中。
    for (const std::string &token : query.plain) {
        if (!signaturePossible(index.nameSigUnion, token)
            && !signaturePossible(index.dirSigUnion, token))
            return results;
    }
    for (const std::string &token : query.path) {
        if (hasPathWildcard(token))
            continue;
        if (!signaturePossible(index.dirSigUnion, token))
            return results;
    }

    bool extAllowed[256] = {};
    const bool hasExtFilter = !query.exts.empty();
    if (hasExtFilter) {
        for (const std::string &token : query.exts)
            extAllowed[extensionTokenHash(token)] = true;
    }

    // 只有路径 / 扩展名条件时不需要按文件名检索：先定位目录，再枚举其中的文件。
    const bool dirDriven = query.plain.empty() && query.regex.empty()
                           && (!query.path.empty() || !query.fullPath.empty() || hasExtFilter);
    if (dirDriven) {
        // 先按目录算一次完整路径并对命中结果做位图，避免对每个文件重复拼接父路径
        std::vector<uint8_t> dirHit;
        std::string dirPath;
        std::vector<uint32_t> ancestors;
        if (!query.path.empty() || !query.fullPath.empty()) {
            dirHit.assign(index.dirs.size(), 0);
            for (uint32_t d = 0; d < index.dirs.size() && !isCancelled(); ++d) {
                if (!pathMask.empty() && !index.dirSigs[d].contains(pathMask))
                    continue;
                if (!buildDirPathInto(index, d, dirPath, ancestors))
                    continue;
                uint8_t matched = 1;
                for (const std::string &token : query.path) {
                    if (!pathTokenMatches(dirPath.data(), dirPath.size(), token)) {
                        matched = 0;
                        break;
                    }
                }
                // full: 关键字先用目录部分粗筛，跨到文件名的部分留给逐文件确认
                for (size_t k = 0; matched && k < query.fullPath.size(); ++k) {
                    const std::string &token = query.fullPath[k];
                    const size_t sep = token.find_last_of("\\/");
                    if (sep == std::string::npos || sep == 0)
                        continue;
                    if (!pathTextMatches(dirPath.data(), dirPath.size(), token.data(), sep))
                        matched = 0;
                }
                dirHit[d] = matched;
            }
        }

        std::string nameBuffer;
        std::string dirBuffer;
        std::string fullPathBuffer;
        std::vector<uint32_t> ancestorBuffer;
        uint32_t dirCacheId = kInvalidIndex;

        // 文件名命中的结果排在“只是目录命中”的前面：D:\*测试* 这种查询里，
        // 明显是文件名里带“测试”的那个才是用户要找的。
        // 文件名命中内部再按命中位置排序：名字以关键字开头的优先，
        // 否则索引按文件名排序时，“6 大气…测试报告”会排在“测试功率…”前面。
        std::vector<std::string> nameLiterals;
        nameLiterals.reserve(query.fullPath.size());
        for (const std::string &token : query.fullPath) {
            const size_t sep = token.find_last_of("\\/");
            const size_t offset = sep == std::string::npos ? 0 : sep + 1;
            nameLiterals.push_back(offset < token.size()
                                       ? longestLiteralRun(token.substr(offset))
                                       : std::string());
        }
        const bool rankByName = !query.fullPath.empty();

        struct NameCandidate
        {
            uint32_t fileIndex;
            uint32_t position; // 关键字在文件名里的位置，越小越靠前
        };
        auto betterCandidate = [](const NameCandidate &a, const NameCandidate &b) {
            if (a.position != b.position)
                return a.position < b.position;
            // 位置相同就保持索引本来的文件名排序，结果顺序更稳定
            return a.fileIndex < b.fileIndex;
        };
        // 堆顶是当前最差的一条，堆大小只保留 limit 条，避免宽泛通配符吃内存
        std::priority_queue<NameCandidate, std::vector<NameCandidate>,
                            decltype(betterCandidate)>
            ranked(betterCandidate);

        std::vector<uint32_t> nameHits;
        std::vector<uint32_t> dirOnlyHits;
        nameHits.reserve(limit);
        dirOnlyHits.reserve(limit);
        for (uint32_t i = 0; i < count; ++i) {
            if ((i & 0x3FFF) == 0 && isCancelled())
                break;
            const FileEntry &file = index.files[i];
            if (!dirHit.empty() && (file.dirId >= dirHit.size() || !dirHit[file.dirId]))
                continue;
            if (!query.fullPath.empty() || hasExtFilter) {
                if (!decodeFileNameInto(index, i, nameBuffer))
                    continue;
                if (!matchesFilters(index, file, nameBuffer.data(), nameBuffer.size(), query,
                                    false, dirBuffer, fullPathBuffer, ancestorBuffer, dirCacheId))
                    continue;
            }

            bool byName = false;
            for (size_t k = 0; !byName && k < query.fullPath.size(); ++k) {
                const std::string &token = query.fullPath[k];
                const size_t sep = token.find_last_of("\\/");
                const size_t offset = sep == std::string::npos ? 0 : sep + 1;
                if (offset >= token.size())
                    continue;
                byName = pathTextMatches(nameBuffer.data(), nameBuffer.size(),
                                         token.data() + offset, token.size() - offset);
            }
            if (byName) {
                if (!rankByName) {
                    nameHits.push_back(i);
                    if (nameHits.size() >= limit)
                        break;
                } else {
                    uint32_t bestPosition =
                        literalPositionNoCase(nameBuffer.data(), nameBuffer.size(),
                                              nameLiterals.front());
                    for (size_t k = 1; k < nameLiterals.size(); ++k) {
                        const uint32_t position =
                            literalPositionNoCase(nameBuffer.data(), nameBuffer.size(),
                                                  nameLiterals[k]);
                        if (position < bestPosition)
                            bestPosition = position;
                    }
                    ranked.push(NameCandidate{i, bestPosition});
                    if (ranked.size() > limit)
                        ranked.pop();
                }
            } else if (dirOnlyHits.size() < limit) {
                dirOnlyHits.push_back(i);
            }
        }

        if (rankByName) {
            nameHits.resize(ranked.size());
            size_t write = nameHits.size();
            while (!ranked.empty() && write > 0) {
                nameHits[--write] = ranked.top().fileIndex;
                ranked.pop();
            }
        }

        for (uint32_t idx : nameHits) {
            add(idx);
            if (full())
                return results;
        }
        for (uint32_t idx : dirOnlyHits) {
            add(idx);
            if (full())
                return results;
        }
        return results;
    }

    std::string dirBuffer;
    std::string fullPathBuffer;
    std::vector<uint32_t> ancestorBuffer;
    uint32_t dirCacheId = kInvalidIndex;
    NameCursor cursor(index);

    // 1. 完整文件名命中（用户输入带扩展名的全名时最快）
    if (query.plain.size() == 1) {
        const std::string &needle = query.plain.front();
        uint32_t pos = static_cast<uint32_t>(lowerBoundName(index, needle));
        cursor.seekBefore(pos);
        while (pos < count) {
            const std::string &name = cursor.next();
            if (compareNameToNeedle(name, needle) != 0)
                break;
            const FileEntry &file = index.files[pos];
            if (pathMask.empty() || index.dirSigs[file.dirId].contains(pathMask)) {
                if (matchesFilters(index, file, name.data(), name.size(), query, false,
                                   dirBuffer, fullPathBuffer, ancestorBuffer, dirCacheId))
                    add(pos);
                if (full())
                    return results;
            }
            ++pos;
        }
    }


    // 2. 最长关键字的词首匹配
    const std::string *primary = nullptr;
    for (const std::string &token : query.plain) {
        if (primary == nullptr || token.size() > primary->size())
            primary = &token;
    }

    if (primary != nullptr) {
        uint32_t pos = static_cast<uint32_t>(lowerBoundName(index, *primary));
        cursor.seekBefore(pos);
        while (pos < count) {
            const std::string &name = cursor.next();
            if (!startsWithNoCase(name.data(), name.size(), *primary))
                break;
            const FileEntry &file = index.files[pos];
            if (pathMask.empty() || index.dirSigs[file.dirId].contains(pathMask)) {
                if (matchesFilters(index, file, name.data(), name.size(), query, false,
                                   dirBuffer, fullPathBuffer, ancestorBuffer, dirCacheId))
                    add(pos);
                if (full())
                    return results;
            }
            ++pos;
        }
    }

    // 3+4. 一次全表扫描同时处理“文件名子串”和“目录路径命中”。
    //      单字符只做前缀，避免第一下按键就扫全库。
    //      文件名命中的结果优先，纯目录命中的结果排在后面。
    const uint64_t mask = querySignature(query.plain);

    bool anyPathPossible = false;
    for (const std::string &token : query.plain) {
        if (signaturePossible(index.dirSigUnion, token)) {
            anyPathPossible = true;
            break;
        }
    }
    const bool allowPathTokens = matchPath && !query.plain.empty() && anyPathPossible;
    const size_t trackedTokens = (std::min)(query.plain.size(), size_t(32));
    const bool allTracked = trackedTokens == query.plain.size();
    const uint32_t allBits = trackedTokens >= 32
                                 ? 0xFFFFFFFFu
                                 : (trackedTokens == 0 ? 0u : ((1u << trackedTokens) - 1u));
    const bool exactPathNeeded = !query.path.empty() || !query.fullPath.empty()
                                 || !query.regex.empty()
                                 || (allowPathTokens && !allTracked);

    // 目录级预匹配：先算出每个目录真正命中哪几个关键字，文件扫描只看 4 字节位掩码。
    // 目录按“父目录先出现”的顺序排列，子目录可以直接继承父目录的命中位；
    // 叶子名再做一次精确匹配，避免 Bloom 误判导致的整目录误命中。
    std::vector<uint32_t> dirMatches;
    if (allowPathTokens) {
        dirMatches.assign(index.dirs.size(), 0);
        std::string dirPath;
        std::vector<uint32_t> dirStack;
        for (size_t k = 0; k < trackedTokens; ++k) {
            const std::string &token = query.plain[k];
            const uint32_t bit = 1u << k;
            const bool spansSeparator = token.find_first_of("\\/") != std::string::npos;
            for (uint32_t d = 0; d < dirMatches.size(); ++d) {
                const DirEntry &dir = index.dirs[d];
                if (!spansSeparator && dir.parent != kInvalidIndex
                    && (dirMatches[dir.parent] & bit) != 0) {
                    dirMatches[d] |= bit;
                    continue;
                }
                if (spansSeparator) {
                    if (!plainDirMasks[k].empty()
                        && !index.dirSigs[d].contains(plainDirMasks[k]))
                        continue;
                    if (!buildDirPathInto(index, d, dirPath, dirStack))
                        continue;
                    if (containsNoCase(dirPath.data(), dirPath.size(), token.data(), token.size()))
                        dirMatches[d] |= bit;
                    continue;
                }
                if (!plainDirMasks[k].empty() && !index.dirSigs[d].contains(plainDirMasks[k]))
                    continue; // 路径签名里连 bigram 都没有，叶子名不可能命中
                const char *leaf = index.dirNames.data() + dir.nameOff;
                if (containsNoCase(leaf, dir.nameLen, token.data(), token.size()))
                    dirMatches[d] |= bit;
            }
        }
    }

    if (!singleChar) {
        std::string nameBuffer;
        nameBuffer.reserve(256);
        std::vector<uint32_t> pathOnly; // 纯目录命中，排在文件名命中之后
        pathOnly.reserve(limit);

        for (uint32_t i = 0; i < count; ++i) {
            if ((i & 0x3FFF) == 0 && isCancelled())
                break;
            // 正则里的固定字面量先在签名上过一遍，避免对全库文件逐个跑 std::regex
            bool regexExcluded = false;
            for (uint64_t regexMask : regexMasks) {
                if ((index.sig[i] & regexMask) != regexMask) {
                    regexExcluded = true;
                    break;
                }
            }
            if (regexExcluded)
                continue;
            if (mask != 0 && (index.sig[i] & mask) != mask) {
                // 文件名签名已经排除所有关键字时，只有当目录全部命中才可能入选。
                if (!allowPathTokens || !allTracked)
                    continue;
                const uint32_t dirId = index.files[i].dirId;
                if (dirId >= dirMatches.size() || (dirMatches[dirId] & allBits) != allBits)
                    continue;
            }

            const FileEntry &file = index.files[i];
            const uint32_t dirBits =
                allowPathTokens && file.dirId < dirMatches.size() ? dirMatches[file.dirId] : 0;

            bool plausible = true;
            for (size_t k = 0; k < plainMasks.size(); ++k) {
                const uint64_t tokenMask = plainMasks[k];
                if (tokenMask == 0 || (index.sig[i] & tokenMask) == tokenMask)
                    continue;
                if (allowPathTokens && k < trackedTokens && (dirBits & (1u << k)) != 0)
                    continue;
                plausible = false;
                break;
            }
            if (!plausible)
                continue;
            if (!pathMask.empty() && !index.dirSigs[file.dirId].contains(pathMask))
                continue;
            if (hasExtFilter && !extAllowed[file.meta])
                continue;
            if (!decodeFileNameInto(index, i, nameBuffer))
                continue;

            if (exactPathNeeded) {
                if (matchesFilters(index, file, nameBuffer.data(), nameBuffer.size(), query,
                                   allowPathTokens, dirBuffer, fullPathBuffer, ancestorBuffer,
                                   dirCacheId)) {
                    add(i);
                    if (full())
                        return results;
                }
                continue;
            }

            bool matched = true;
            if (hasExtFilter
                && !fileExtensionMatches(nameBuffer.data(), nameBuffer.size(), query.exts))
                matched = false;
            bool byName = true;
            for (size_t k = 0; matched && k < query.plain.size(); ++k) {
                const std::string &token = query.plain[k];
                if (containsNoCase(nameBuffer.data(), nameBuffer.size(), token.data(),
                                   token.size()))
                    continue;
                if (allowPathTokens && k < trackedTokens && (dirBits & (1u << k)) != 0) {
                    byName = false;
                    continue;
                }
                matched = false;
                break;
            }
            if (!matched)
                continue;
            if (byName) {
                add(i);
                if (full())
                    return results;
            } else if (pathOnly.size() < limit) {
                pathOnly.push_back(i);
            }
        }

        for (uint32_t idx : pathOnly) {
            add(idx);
            if (full())
                return results;
        }
    }

    return results;
}

} // namespace filesearch
