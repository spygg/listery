#pragma once

#include <cstdint>
#include <functional>
#include <regex>
#include <string>
#include <vector>

namespace filesearch {

constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

/** 文件名前缀压缩的块大小：每个块的第一条存完整名字。 */
constexpr uint32_t kNameBlock = 16;

/** 24 位大小的上限，超过这个值统一显示为 >=16MB。 */
constexpr uint32_t kSizeCap = 0xFFFFFFu;

/**
 * 一个文件条目，12 字节。
 *
 * files 数组本身按文件名排序（不区分大小写），所以检索时可以直接对 files 做二分。
 * nameOff 指向 names 里的一条前缀压缩记录：[prefixLen:1][suffix][\0]。
 * meta 在扫描/排序阶段是文件名长度（长于 255 字节时写 255），
 * finalize 之后改成扩展名的 8 位哈希：0 = 没有扩展名，1..255 = 哈希值。
 * size 是 24 位字节数，0xFFFFFF 表示 >=16MB。
 */
struct FileEntry
{
    uint32_t nameOff = 0;
    uint32_t dirId = 0;
    uint8_t size[3] = {0, 0, 0};
    uint8_t meta = 0;
};
static_assert(sizeof(FileEntry) == 12, "FileEntry must stay 12 bytes");

inline uint32_t fileSize(const FileEntry &file)
{
    return static_cast<uint32_t>(file.size[0])
           | (static_cast<uint32_t>(file.size[1]) << 8)
           | (static_cast<uint32_t>(file.size[2]) << 16);
}

inline void setFileSize(FileEntry &file, uint32_t bytes)
{
    const uint32_t clamped = bytes < kSizeCap ? bytes : kSizeCap;
    file.size[0] = static_cast<uint8_t>(clamped & 0xFF);
    file.size[1] = static_cast<uint8_t>((clamped >> 8) & 0xFF);
    file.size[2] = static_cast<uint8_t>((clamped >> 16) & 0xFF);
}

/**
 * 目录表：只存叶子目录名，完整路径由 parent 链拼出来。
 * 根目录的 name 是完整根路径（例如 C:\Users 或 D:\）。
 */
struct DirEntry
{
    uint32_t nameOff = 0;
    uint16_t nameLen = 0;
    uint16_t depth = 0;
    uint32_t parent = kInvalidIndex;
};
static_assert(sizeof(DirEntry) == 12, "DirEntry must stay 12 bytes");

/** 目录路径用 256 位签名，长路径下比 64 位 Bloom 更不容易饱和。 */
struct Signature256
{
    uint64_t words[4] = {0, 0, 0, 0};

    bool empty() const
    {
        return (words[0] | words[1] | words[2] | words[3]) == 0;
    }

    bool contains(const Signature256 &mask) const
    {
        return (words[0] & mask.words[0]) == mask.words[0]
               && (words[1] & mask.words[1]) == mask.words[1]
               && (words[2] & mask.words[2]) == mask.words[2]
               && (words[3] & mask.words[3]) == mask.words[3];
    }

    void merge(const Signature256 &other)
    {
        for (int i = 0; i < 4; ++i)
            words[i] |= other.words[i];
    }
};

struct Index
{
    std::vector<FileEntry> files;   // 按文件名排序
    std::vector<DirEntry> dirs;
    std::vector<char> names;        // 前缀压缩后的文件名记录
    std::vector<char> dirNames;     // 目录叶子名
    std::vector<uint64_t> sig;      // 每个文件名的 bigram 签名
    std::vector<Signature256> dirSigs; // 每个目录完整路径的签名
    uint64_t nameSigUnion = 0;
    Signature256 dirSigUnion;
    uint64_t builtAt = 0; // Unix 秒
    uint64_t scanMs = 0;

    /** 目录叶子名。根目录返回完整根路径。 */
    const char *dirName(uint32_t id) const
    {
        if (id >= dirs.size())
            return "";
        return dirNames.data() + dirs.at(id).nameOff;
    }

    uint16_t dirNameLen(uint32_t id) const
    {
        if (id >= dirs.size())
            return 0;
        return dirs.at(id).nameLen;
    }
};

/**
 * 把扫描阶段的原始数组整理成检索结构：
 *  - 按文件名排序（直接排序 files，不再保留第二份索引）；
 *  - 生成前缀压缩的 names；
 *  - 生成每个文件名的签名和每个目录的路径签名。
 *
 * rawFiles[i].nameOff / meta 指向 rawNames；meta == 255 时长名以 \0 结尾。
 */
void finalizeIndex(Index &index,
                   std::vector<FileEntry> rawFiles,
                   std::vector<DirEntry> dirs,
                   std::vector<char> rawNames,
                   std::vector<char> dirNames,
                   std::vector<Signature256> dirSigs);

/** 解码第 i 个文件名到 out。 */
bool fileNameAt(const Index &index, uint32_t fileIndex, std::string &out);

/** 拼接目录完整路径到 out（不含文件名）。 */
bool buildDirPath(const Index &index, uint32_t dirId, std::string &out);

// 二进制快照。失败时返回 false，error 写入原因（可为 nullptr）。
bool loadIndex(const std::wstring &path, Index &out, std::wstring *error);
bool saveIndex(const std::wstring &path, const Index &index, std::wstring *error);

struct ParsedQuery
{
    std::vector<std::string> plain;     // 文件名（默认）/ 路径（allowPath 时）
    std::vector<std::string> path;      // path:xxx，强制匹配目录
    std::vector<std::string> fullPath;  // full:xxx 或带分隔符的裸词，匹配完整路径
    std::vector<std::regex> regex;      // re:xxx，不区分大小写的文件名正则
    std::vector<std::string> regexKeys; // 与 regex 一一对应：正则里必须出现的字面量
    std::vector<std::string> exts;      // ext:pdf 或 *.pdf
    std::string regexError;             // 正则语法错误说明（UTF-8）

    bool empty() const
    {
        return plain.empty() && path.empty() && fullPath.empty() && regex.empty() && exts.empty();
    }
};

ParsedQuery parseQuery(const std::string &utf8Query);

/**
 * 检索。结果按“完全匹配 → 前缀 → 子串 → 路径命中”的顺序返回，最多 maxResults 条。
 * matchPath=false 时只匹配文件名；true 时在前两轮没填满的情况下再匹配目录路径。
 * cancelled 可选：返回 true 时尽快放弃（长正则扫描时用来响应输入变化）。
 */
std::vector<uint32_t> searchIndex(const Index &index, const ParsedQuery &query,
                                  int maxResults, bool matchPath,
                                  const std::function<bool()> &cancelled = std::function<bool()>());

/** 计算一段文本（文件名或路径）的 64 位 bigram 签名。 */
uint64_t textSignature(const char *text, size_t len);

/** 计算目录路径的 256 位签名。 */
Signature256 textSignature256(const char *text, size_t len);

std::string wideToUtf8(const std::wstring &text);
std::wstring utf8ToWide(const std::string &text);
std::string formatSize(uint32_t bytes);

} // namespace filesearch
