#include "file_scanner.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#ifndef FIND_FIRST_EX_LARGE_FETCH
#  define FIND_FIRST_EX_LARGE_FETCH 2
#endif

namespace filesearch {
namespace {

std::wstring lowerWide(const std::wstring &text)
{
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text)
        out.push_back(static_cast<wchar_t>(std::towlower(ch)));
    return out;
}

bool endsWithSeparator(const std::wstring &path)
{
    return !path.empty() && (path.back() == L'\\' || path.back() == L'/');
}

std::wstring joinPath(const std::wstring &dir, const std::wstring &name)
{
    if (dir.empty())
        return name;
    if (endsWithSeparator(dir))
        return dir + name;
    return dir + L"\\" + name;
}

std::wstring toLongPath(const std::wstring &path)
{
    if (path.rfind(L"\\\\?\\", 0) == 0)
        return path;
    if (path.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring toDisplayPath(const std::wstring &path)
{
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return L"\\\\" + path.substr(8);
    if (path.rfind(L"\\\\?\\", 0) == 0)
        return path.substr(4);
    return path;
}

bool isUnderOrEqual(const std::wstring &lowerPath, const std::wstring &lowerRoot)
{
    if (lowerRoot.empty())
        return false;
    if (lowerPath.size() < lowerRoot.size())
        return false;
    if (lowerPath.compare(0, lowerRoot.size(), lowerRoot) != 0)
        return false;
    if (lowerPath.size() == lowerRoot.size())
        return true;
    if (lowerRoot.back() == L'\\' || lowerRoot.back() == L'/')
        return true;
    const wchar_t next = lowerPath[lowerRoot.size()];
    return next == L'\\' || next == L'/';
}

bool isExcludedPath(const std::wstring &path, const std::vector<std::wstring> &prefixes)
{
    if (prefixes.empty())
        return false;
    const std::wstring lower = lowerWide(path);
    for (const std::wstring &prefix : prefixes) {
        if (isUnderOrEqual(lower, prefix))
            return true;
    }
    return false;
}

} // namespace
bool scanFileSystem(const ScanConfig &config, Index &out, std::atomic<bool> &stop,
                    const ScanLog &log)
{
    std::vector<FileEntry> rawFiles;
    std::vector<DirEntry> dirs;
    std::vector<char> dirNames;
    std::vector<Signature256> dirSigs;
    std::vector<char> rawNames;
    rawFiles.reserve(1u << 18);
    rawNames.reserve(1u << 22);
    dirs.reserve(1u << 14);
    dirNames.reserve(1u << 20);
    dirSigs.reserve(1u << 14);

    std::vector<std::wstring> excludeNames = config.excludeNames;
    std::sort(excludeNames.begin(), excludeNames.end());
    excludeNames.erase(std::unique(excludeNames.begin(), excludeNames.end()), excludeNames.end());

    std::vector<std::wstring> excludePrefixes = config.excludePrefixes;
    std::sort(excludePrefixes.begin(), excludePrefixes.end());

    auto addDirectory = [&dirs, &dirNames, &dirSigs](const std::wstring &path, uint32_t parent,
                                                     uint16_t depth) -> uint32_t {
        const std::string utf8 = wideToUtf8(path);
        if (utf8.empty() || utf8.size() > 65535)
            return kInvalidIndex;

        // 子目录只保存叶子名，完整路径由 parent 链拼出来；根目录保存完整根路径。
        std::string leaf;
        if (parent == kInvalidIndex) {
            leaf = utf8;
        } else {
            const size_t slash = utf8.find_last_of("\\/");
            leaf = slash == std::string::npos ? utf8 : utf8.substr(slash + 1);
            if (leaf.empty())
                leaf = utf8;
        }
        if (leaf.size() > 65535)
            return kInvalidIndex;

        DirEntry entry;
        entry.nameOff = static_cast<uint32_t>(dirNames.size());
        entry.nameLen = static_cast<uint16_t>(leaf.size());
        entry.depth = depth;
        entry.parent = parent;
        dirNames.insert(dirNames.end(), leaf.begin(), leaf.end());
        dirSigs.push_back(textSignature256(utf8.data(), utf8.size()));
        dirs.push_back(entry);
        return static_cast<uint32_t>(dirs.size() - 1);
    };

    struct Pending
    {
        std::wstring path;
        uint32_t dirId = kInvalidIndex;
        uint16_t depth = 0;
    };
    std::vector<Pending> stack;

    // 去掉互相包含的根目录：D:\ 已经覆盖 D:\Projects，不需要扫两遍。
    std::vector<std::wstring> roots = config.roots;
    std::sort(roots.begin(), roots.end(), [](const std::wstring &a, const std::wstring &b) {
        return a.size() < b.size();
    });
    std::vector<std::wstring> rootKeys;
    std::vector<std::wstring> effectiveRoots;
    for (const std::wstring &root : roots) {
        const std::wstring key = lowerWide(root);
        bool covered = false;
        for (const std::wstring &existing : rootKeys) {
            if (isUnderOrEqual(key, existing)) {
                covered = true;
                break;
            }
        }
        if (covered)
            continue;
        rootKeys.push_back(key);
        effectiveRoots.push_back(root);
    }

    uint64_t scannedFiles = 0;
    uint64_t scannedDirs = 0;
    uint64_t accessErrors = 0;
    auto lastLog = std::chrono::steady_clock::now();

    for (const std::wstring &root : effectiveRoots) {
        if (stop.load())
            return false;
        const std::wstring longRoot = toLongPath(root);
        const DWORD attrs = GetFileAttributesW(longRoot.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            ++accessErrors;
            if (log)
                log("跳过不可访问的根目录：" + wideToUtf8(root));
            continue;
        }
        const uint32_t rootId = addDirectory(root, kInvalidIndex, 0);
        if (rootId == kInvalidIndex)
            continue;
        stack.push_back(Pending{root, rootId, 0});
        ++scannedDirs;
    }

    while (!stack.empty()) {
        if (stop.load())
            return false;

        Pending current = std::move(stack.back());
        stack.pop_back();

        std::wstring pattern = toLongPath(current.path);
        if (!endsWithSeparator(pattern))
            pattern.push_back(L'\\');
        pattern.push_back(L'*');

#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0601
        const FINDEX_INFO_LEVELS infoLevel = FindExInfoBasic;
        const DWORD extraFlags = FIND_FIRST_EX_LARGE_FETCH;
#else
        const FINDEX_INFO_LEVELS infoLevel = FindExInfoStandard;
        const DWORD extraFlags = 0;
#endif

        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(pattern.c_str(), infoLevel, &data, FindExSearchNameMatch,
                                       nullptr, extraFlags);
        if (find == INVALID_HANDLE_VALUE) {
            ++accessErrors;
            continue;
        }

        do {
            if (std::wcscmp(data.cFileName, L".") == 0
                || std::wcscmp(data.cFileName, L"..") == 0)
                continue;

            const DWORD attrs = data.dwFileAttributes;
            const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool isHidden = (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
            if (isDirectory && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                continue; // junction / symlink 目录可能成环
            if (!config.includeHidden && isHidden)
                continue;

            const std::wstring name(data.cFileName);
            const std::wstring lowerName = lowerWide(name);

            if (isDirectory) {
                if (std::binary_search(excludeNames.begin(), excludeNames.end(), lowerName))
                    continue;

                const std::wstring full = joinPath(current.path, name);
                if (isExcludedPath(full, excludePrefixes))
                    continue;

                const uint32_t dirId = addDirectory(full, current.dirId,
                                                    static_cast<uint16_t>(current.depth + 1));
                if (dirId == kInvalidIndex)
                    continue;
                stack.push_back(Pending{full, dirId, static_cast<uint16_t>(current.depth + 1)});
                ++scannedDirs;
            } else {
                const std::string utf8 = wideToUtf8(name);
                if (utf8.empty() || utf8.size() > 65535)
                    continue;

                FileEntry entry;
                entry.nameOff = static_cast<uint32_t>(rawNames.size());
                entry.meta = utf8.size() >= 255 ? 255
                                                : static_cast<uint8_t>(utf8.size());
                entry.dirId = current.dirId;
                setFileSize(entry, static_cast<uint32_t>(
                                       (std::min)((static_cast<ULONGLONG>(data.nFileSizeHigh) << 32)
                                                      | data.nFileSizeLow,
                                                  static_cast<ULONGLONG>(kSizeCap))));
                rawNames.insert(rawNames.end(), utf8.begin(), utf8.end());
                if (utf8.size() >= 255)
                    rawNames.push_back('\0');
                rawFiles.push_back(entry);
                ++scannedFiles;

                if ((scannedFiles & 0x3FFFu) == 0) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastLog > std::chrono::seconds(2)) {
                        lastLog = now;
                        if (log)
                            log("扫描中：" + std::to_string(scannedFiles) + " 个文件，"
                                + std::to_string(scannedDirs) + " 个目录");
                    }
                }
            }

            if ((scannedFiles & 0xFFFFu) == 0 && stop.load())
                break;
        } while (FindNextFileW(find, &data));

        const DWORD lastError = GetLastError();
        FindClose(find);
        if (lastError != ERROR_NO_MORE_FILES && lastError != ERROR_SUCCESS)
            ++accessErrors;
    }

    if (stop.load())
        return false;

    if (log)
        log("遍历完成：" + std::to_string(scannedFiles) + " 个文件，"
            + std::to_string(scannedDirs) + " 个目录，"
            + std::to_string(accessErrors) + " 个目录访问失败");

    finalizeIndex(out, std::move(rawFiles), std::move(dirs), std::move(rawNames),
                  std::move(dirNames), std::move(dirSigs));
    out.builtAt = static_cast<uint64_t>(std::time(nullptr));
    return true;
}

} // namespace filesearch
