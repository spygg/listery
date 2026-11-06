/*
 * filesearch_bench：文件搜索索引的开发基准工具。
 *
 * 用法：
 *   filesearch_bench [root ...] [--query 关键字] [--iterations N] [--save 文件] [--load 文件]
 *
 * 不带 root 时扫描当前目录。输出扫描耗时、索引内存占用以及每次检索的耗时。
 */

#include "file_index.h"
#include "file_scanner.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace {

std::wstring fromUtf8(const std::string &text)
{
    return filesearch::utf8ToWide(text);
}

double elapsedMs(std::chrono::steady_clock::time_point start,
                 std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

size_t indexBytes(const filesearch::Index &index)
{
    return index.files.size() * sizeof(filesearch::FileEntry)
           + index.dirs.size() * sizeof(filesearch::DirEntry)
           + index.names.size() + index.dirNames.size()
           + index.sig.size() * sizeof(uint64_t)
           + index.dirSigs.size() * sizeof(filesearch::Signature256);
}

std::wstring currentDirectory()
{
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = GetCurrentDirectoryW(MAX_PATH * 4, buffer);
    return length > 0 ? std::wstring(buffer, length) : std::wstring(L".");
}

void printUsage()
{
    std::printf("用法: filesearch_bench [root ...] [--query 关键字] [--iterations N] "
                "[--limit N] [--save 文件] [--load 文件] [--exclude-name 名称] "
                "[--exclude-prefix 路径] [--include-hidden]\n");
}

} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        std::printf("无法读取命令行参数\n");
        return 1;
    }

    filesearch::ScanConfig config;
    std::vector<std::string> queries;
    int iterations = 5;
    int limit = 12;
    std::wstring savePath;
    std::wstring loadPath;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h") {
            printUsage();
            LocalFree(argv);
            return 0;
        }
        if (arg == L"--query" && i + 1 < argc) {
            queries.push_back(filesearch::wideToUtf8(argv[++i]));
            continue;
        }
        if (arg == L"--iterations" && i + 1 < argc) {
            iterations = static_cast<int>(wcstol(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == L"--limit" && i + 1 < argc) {
            limit = static_cast<int>(wcstol(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == L"--save" && i + 1 < argc) {
            savePath = argv[++i];
            continue;
        }
        if (arg == L"--exclude-name" && i + 1 < argc) {
            std::wstring name = argv[++i];
            for (wchar_t &ch : name)
                ch = static_cast<wchar_t>(std::towlower(ch));
            config.excludeNames.push_back(name);
            continue;
        }
        if (arg == L"--exclude-prefix" && i + 1 < argc) {
            std::wstring prefix = argv[++i];
            for (wchar_t &ch : prefix)
                ch = static_cast<wchar_t>(std::towlower(ch));
            config.excludePrefixes.push_back(prefix);
            continue;
        }
        if (arg == L"--include-hidden") {
            config.includeHidden = true;
            continue;
        }
        if (arg == L"--load" && i + 1 < argc) {
            loadPath = argv[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == L'-') {
            std::printf("未知参数: %s\n", filesearch::wideToUtf8(arg).c_str());
            printUsage();
            LocalFree(argv);
            return 2;
        }
        config.roots.push_back(arg);
    }
    LocalFree(argv);

    if (config.roots.empty())
        config.roots.push_back(currentDirectory());
    if (queries.empty())
        queries = {"a", "test", "chrome", "setup", "pdf", "src"};

    filesearch::Index index;

    if (!loadPath.empty()) {
        const auto started = std::chrono::steady_clock::now();
        std::wstring error;
        if (!filesearch::loadIndex(loadPath, index, &error)) {
            std::printf("加载索引失败: %s\n", filesearch::wideToUtf8(error).c_str());
            return 1;
        }
        const auto finished = std::chrono::steady_clock::now();
        std::printf("加载快照 %.1f ms\n", elapsedMs(started, finished));
    } else {
        std::atomic<bool> stop{false};
        const auto started = std::chrono::steady_clock::now();
        const auto log = [](const std::string &message) {
            std::printf("  %s\n", message.c_str());
            std::fflush(stdout);
        };
        if (!filesearch::scanFileSystem(config, index, stop, log)) {
            std::printf("扫描失败\n");
            return 1;
        }
        const auto finished = std::chrono::steady_clock::now();
        std::printf("扫描完成 %.1f ms\n", elapsedMs(started, finished));
    }

    std::printf("文件 %zu，目录 %zu，索引约 %.1f MB\n", index.files.size(), index.dirs.size(),
                static_cast<double>(indexBytes(index)) / (1024.0 * 1024.0));

    if (!savePath.empty()) {
        const auto started = std::chrono::steady_clock::now();
        std::wstring error;
        if (!filesearch::saveIndex(savePath, index, &error))
            std::printf("保存失败: %s\n", filesearch::wideToUtf8(error).c_str());
        else
            std::printf("保存快照 %.1f ms\n", elapsedMs(started, std::chrono::steady_clock::now()));
    }

    if (iterations < 1)
        iterations = 1;

    for (const std::string &text : queries) {
        const filesearch::ParsedQuery query = filesearch::parseQuery(text);
        double best = 1e300;
        double total = 0.0;
        std::vector<uint32_t> hits;
        for (int i = 0; i < iterations; ++i) {
            const auto started = std::chrono::steady_clock::now();
            hits = filesearch::searchIndex(index, query, limit, true);
            const double ms = elapsedMs(started, std::chrono::steady_clock::now());
            best = (std::min)(best, ms);
            total += ms;
        }
        std::printf("查询 \"%s\": 最佳 %.2f ms，平均 %.2f ms，%zu 条", text.c_str(), best,
                    total / iterations, hits.size());
        if (!hits.empty()) {
            std::string name;
            if (filesearch::fileNameAt(index, hits.front(), name))
                std::printf("，首条：%s", name.c_str());
        }
        if (std::getenv("FILESEARCH_DEBUG") != nullptr) {
            std::printf("\n");
            for (uint32_t hit : hits) {
                std::string name;
                if (!filesearch::fileNameAt(index, hit, name))
                    continue;
                std::string dir;
                filesearch::buildDirPath(index, index.files[hit].dirId, dir);
                std::printf("    %s\\%s\n", dir.c_str(), name.c_str());
            }
        }
        std::printf("\n");
    }
    return 0;
}
