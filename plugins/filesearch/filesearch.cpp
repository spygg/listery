/*
 * Listery 文件搜索插件（路线 B：自建紧凑索引）
 *
 *  - 后台线程扫描用户目录、Public 和非系统固定磁盘；
 *  - 内存索引按文件名排序，检索走“全名 → 前缀 → 子串 → 路径”；
 *  - 索引二进制快照保存在 %LOCALAPPDATA%\Listery\FileSearch\index.bin；
 *  - 配置文件 filesearch.ini 与快照同目录，可用 !config 打开；
 *  - 面板里输入 !status / !reindex / !config 查看状态或重建索引。
 *
 * 插件不依赖 Qt，也不依赖 Everything。
 */

#include "file_index.h"
#include "file_scanner.h"

#include "listery_plugin_api.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using filesearch::DirEntry;
using filesearch::FileEntry;
using filesearch::Index;
using filesearch::ParsedQuery;
using filesearch::ScanConfig;

constexpr int kDefaultMaxResults = 12;
constexpr int kDefaultRescanMinutes = 60;
constexpr uint32_t kMaxResultsCap = 100;

// ---------------------------------------------------------------------------
// 插件全局状态
// ---------------------------------------------------------------------------

const ListeryHostV1 *g_host = nullptr;

std::mutex g_stateMutex;
std::shared_ptr<const Index> g_index;
std::string g_status = "索引尚未建立";
/** 磁盘上有可用的索引快照；索引被闲置卸载后，靠它决定是重新加载还是重建。 */
bool g_haveSnapshot = false;
uint64_t g_builtAt = 0;

std::wstring g_dataDir;
std::wstring g_indexPath;
std::wstring g_configPath;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_forceScan{false};
std::atomic<bool> g_matchPath{true};
std::atomic<int> g_rescanMinutes{kDefaultRescanMinutes};
/** 闲置多少分钟后卸载内存索引（0 = 不卸载）。下次查询时从快照重新加载。 */
std::atomic<int> g_idleUnloadMinutes{0};
std::atomic<uint64_t> g_lastQueryAt{0};

struct AppConfig
{
    ScanConfig scan;
    int rescanMinutes = kDefaultRescanMinutes;
    int idleUnloadMinutes = 0;
    bool matchPath = true;
};

std::mutex g_configMutex;
AppConfig g_config;

std::thread g_scanThread;
std::thread g_searchThread;

struct QueryRequest
{
    unsigned long long id = 0;
    std::string text;
    int maxResults = kDefaultMaxResults;
};

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<QueryRequest> g_queue;
std::set<unsigned long long> g_cancelled;
bool g_queueStop = false;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

uint64_t nowUnixSeconds()
{
    return static_cast<uint64_t>(std::time(nullptr));
}

std::wstring trimWide(const std::wstring &text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t' || text[begin] == L'\r'
                           || text[begin] == L'\n'))
        ++begin;
    while (end > begin
           && (text[end - 1] == L' ' || text[end - 1] == L'\t' || text[end - 1] == L'\r'
               || text[end - 1] == L'\n'))
        --end;
    return text.substr(begin, end - begin);
}

std::wstring lowerWide(const std::wstring &text)
{
    std::wstring out = text;
    for (wchar_t &ch : out) {
        if (ch >= L'A' && ch <= L'Z')
            ch = static_cast<wchar_t>(ch + 32);
    }
    return out;
}

std::wstring getEnv(const wchar_t *name)
{
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0)
        return std::wstring();
    std::vector<wchar_t> buffer(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, buffer.data(), needed);
    if (written == 0)
        return std::wstring();
    return std::wstring(buffer.data(), written);
}

std::wstring expandEnv(const std::wstring &text)
{
    if (text.empty())
        return text;
    const DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (needed == 0)
        return text;
    std::vector<wchar_t> buffer(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(text.c_str(), buffer.data(), needed);
    if (written == 0 || written > needed)
        return text;
    return std::wstring(buffer.data(), written - 1);
}

std::wstring normalizePath(const std::wstring &raw)
{
    std::wstring path = trimWide(expandEnv(raw));
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
        path = path.substr(1, path.size() - 2);
    for (wchar_t &ch : path) {
        if (ch == L'/')
            ch = L'\\';
    }
    while (path.size() > 3 && path.back() == L'\\')
        path.pop_back();
    if (path.size() == 2 && path[1] == L':')
        path.push_back(L'\\');
    return path;
}

std::vector<std::wstring> splitList(const std::wstring &text)
{
    std::vector<std::wstring> out;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L';' || ch == L'|' || ch == L'\n') {
            const std::wstring item = normalizePath(current);
            if (!item.empty())
                out.push_back(item);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    const std::wstring item = normalizePath(current);
    if (!item.empty())
        out.push_back(item);
    return out;
}

void addUniquePath(std::vector<std::wstring> &list, std::vector<std::wstring> &keys,
                   const std::wstring &path)
{
    const std::wstring normalized = normalizePath(path);
    if (normalized.empty())
        return;
    const std::wstring key = lowerWide(normalized);
    if (std::find(keys.begin(), keys.end(), key) != keys.end())
        return;
    keys.push_back(key);
    list.push_back(normalized);
}

void setStatus(const std::string &text)
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_status = text;
}

std::string statusText()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_status;
}

void hostLog(int level, const std::string &message)
{
    if (g_host != nullptr && g_host->log != nullptr && !message.empty())
        g_host->log(g_host->context, level, message.c_str());
}

std::string formatCount(uint64_t value)
{
    std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const size_t lead = digits.size() % 3;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (i - lead) % 3 == 0 && i >= lead)
            out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

std::wstring dataDirectory()
{
    std::wstring base = getEnv(L"LOCALAPPDATA");
    if (base.empty()) {
        const std::wstring profile = getEnv(L"USERPROFILE");
        if (!profile.empty())
            base = profile + L"\\AppData\\Local";
    }
    if (base.empty())
        base = L".";
    return base + L"\\Listery\\FileSearch";
}

bool readFileBytes(const std::wstring &path, std::string &out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0
        || size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < out.size()) {
        DWORD read = 0;
        const DWORD request = static_cast<DWORD>((std::min)(out.size() - offset, size_t(1 << 20)));
        if (!ReadFile(file, out.data() + offset, request, &read, nullptr) || read == 0) {
            CloseHandle(file);
            out.clear();
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

bool writeFileBytes(const std::wstring &path, const std::string &data)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>((std::min)(data.size() - offset, size_t(1 << 20)));
        if (!WriteFile(file, data.data() + offset, request, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        offset += written;
    }
    CloseHandle(file);
    if (!ok)
        DeleteFileW(path.c_str());
    return ok;
}

void writeDefaultConfigIfMissing()
{
    const DWORD attrs = GetFileAttributesW(g_configPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES)
        return;

    std::string text;
    text += "\xEF\xBB\xBF"; // UTF-8 BOM，方便记事本正确显示中文
    text += "; Listery 文件搜索插件配置\r\n";
    text += "; 修改后回到搜索框输入 !reindex 重新扫描（或用托盘菜单重启 Listery）。\r\n";
    text += ";\r\n";
    text += "; roots：要索引的根目录，用英文分号分隔；留空 = 用户目录 + Public + 所有非系统固定磁盘\r\n";
    text += ";   例如：roots=C:\\Users;D:\\Projects;E:\\\r\n";
    text += "; exclude_names：额外排除的目录名（只写目录名，不区分大小写）\r\n";
    text += "; exclude_prefixes：额外排除的完整路径前缀，例如 D:\\临时\r\n";
    text += "; include_hidden：是否索引隐藏/系统文件，0 或 1\r\n";
    text += "; match_path：文件名没命中时是否继续匹配目录路径，0 或 1\r\n";
    text += "; rescan_minutes：每隔多少分钟后台重建索引，0 = 只使用现有快照\r\n";
    text += "; idle_unload_minutes：闲置多少分钟后释放内存索引，下次搜索时自动从快照加载；0 = 常驻内存（默认）\r\n";
    text += "\r\n";
    text += "roots=\r\n";
    text += "exclude_names=\r\n";
    text += "exclude_prefixes=\r\n";
    text += "include_hidden=0\r\n";
    text += "match_path=1\r\n";
    text += "rescan_minutes=60\r\n";
    text += "idle_unload_minutes=0\r\n";

    if (!writeFileBytes(g_configPath, text))
        hostLog(LISTERY_LOG_WARNING, "无法写入默认配置文件");
}

std::vector<std::wstring> defaultRoots()
{
    std::vector<std::wstring> roots;
    std::vector<std::wstring> keys;

    addUniquePath(roots, keys, getEnv(L"USERPROFILE"));
    addUniquePath(roots, keys, getEnv(L"PUBLIC"));

    wchar_t windowsDir[MAX_PATH] = {};
    wchar_t systemDrive = L'C';
    if (GetWindowsDirectoryW(windowsDir, MAX_PATH) > 0)
        systemDrive = static_cast<wchar_t>(std::towupper(windowsDir[0]));

    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((drives & (1u << i)) == 0)
            continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        if (letter == systemDrive)
            continue;
        wchar_t root[4] = {letter, L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) != DRIVE_FIXED)
            continue;
        addUniquePath(roots, keys, root);
    }
    return roots;
}

std::vector<std::wstring> defaultExcludeNames()
{
    static const wchar_t *kNames[] = {
        L"$recycle.bin", L"system volume information", L"windows", L"program files",
        L"program files (x86)", L"programdata", L"appdata", L"node_modules", L".git",
        L".svn", L".hg", L"__pycache__", L".venv", L"venv", L".cache", L"cache",
        L"temp", L"tmp", L"$windows.~bt", L"$windows.~ws", L"recovery", L"windows.old",
        L".vs", L".idea", L"perflogs"};
    std::vector<std::wstring> out;
    for (const wchar_t *name : kNames)
        out.push_back(lowerWide(name));
    return out;
}

std::vector<std::wstring> defaultExcludePrefixes()
{
    std::vector<std::wstring> prefixes;
    const wchar_t *vars[] = {L"WINDIR", L"PROGRAMFILES", L"PROGRAMFILES(X86)", L"PROGRAMDATA"};
    for (const wchar_t *name : vars) {
        const std::wstring value = normalizePath(getEnv(name));
        if (!value.empty())
            prefixes.push_back(lowerWide(value));
    }
    if (!g_dataDir.empty())
        prefixes.push_back(lowerWide(normalizePath(g_dataDir)));
    return prefixes;
}

void appendExcludeNames(std::vector<std::wstring> &target, const std::vector<std::wstring> &extra)
{
    for (const std::wstring &name : extra) {
        std::wstring value = lowerWide(normalizePath(name));
        while (!value.empty() && (value.back() == L'\\' || value.back() == L'/'))
            value.pop_back();
        const size_t slash = value.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            value = value.substr(slash + 1);
        if (!value.empty())
            target.push_back(value);
    }
}

int parseInt(const std::string &text, int fallback)
{
    if (text.empty())
        return fallback;
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || value < 0 || value > 100000)
        return fallback;
    return static_cast<int>(value);
}

bool parseBool(const std::string &text, bool fallback)
{
    if (text.empty())
        return fallback;
    std::string lowered = text;
    for (char &ch : lowered) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + 32);
    }
    return lowered != "0" && lowered != "false" && lowered != "no" && lowered != "off";
}

bool loadConfig()
{
    AppConfig config;
    config.scan.roots = defaultRoots();
    config.scan.excludeNames = defaultExcludeNames();
    config.scan.excludePrefixes = defaultExcludePrefixes();
    config.scan.includeHidden = false;
    config.rescanMinutes = kDefaultRescanMinutes;
    config.idleUnloadMinutes = 0;
    config.matchPath = true;

    std::string raw;
    if (readFileBytes(g_configPath, raw)) {
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF
            && static_cast<unsigned char>(raw[1]) == 0xBB
            && static_cast<unsigned char>(raw[2]) == 0xBF) {
            raw.erase(0, 3);
        }

        std::vector<std::pair<std::string, std::string>> entries;
        size_t begin = 0;
        while (begin <= raw.size()) {
            const size_t end = raw.find('\n', begin);
            std::string line = raw.substr(begin, end == std::string::npos
                                                     ? std::string::npos
                                                     : end - begin);
            begin = end == std::string::npos ? raw.size() + 1 : end + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos)
                continue;
            if (line[first] == ';' || line[first] == '#')
                continue;
            const size_t eq = line.find('=', first);
            if (eq == std::string::npos)
                continue;
            std::string key = line.substr(first, eq - first);
            std::string value = line.substr(eq + 1);
            const size_t keyEnd = key.find_last_not_of(" \t");
            key = keyEnd == std::string::npos ? std::string() : key.substr(0, keyEnd + 1);
            const size_t valueBegin = value.find_first_not_of(" \t");
            const size_t valueEnd = value.find_last_not_of(" \t");
            value = valueBegin == std::string::npos
                        ? std::string()
                        : value.substr(valueBegin, valueEnd - valueBegin + 1);
            for (char &ch : key) {
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<char>(ch + 32);
            }
            entries.emplace_back(key, value);
        }

        for (const auto &entry : entries) {
            const std::string &key = entry.first;
            const std::string &value = entry.second;
            if (key == "roots") {
                std::vector<std::wstring> roots = splitList(filesearch::utf8ToWide(value));
                if (!roots.empty()) {
                    config.scan.roots.clear();
                    std::vector<std::wstring> keys;
                    for (const std::wstring &root : roots)
                        addUniquePath(config.scan.roots, keys, root);
                }
            } else if (key == "exclude_names") {
                appendExcludeNames(config.scan.excludeNames,
                                   splitList(filesearch::utf8ToWide(value)));
            } else if (key == "exclude_prefixes") {
                for (const std::wstring &prefix : splitList(filesearch::utf8ToWide(value)))
                    config.scan.excludePrefixes.push_back(lowerWide(prefix));
            } else if (key == "include_hidden") {
                config.scan.includeHidden = parseBool(value, false);
            } else if (key == "match_path") {
                config.matchPath = parseBool(value, true);
            } else if (key == "rescan_minutes") {
                config.rescanMinutes = parseInt(value, kDefaultRescanMinutes);
            } else if (key == "idle_unload_minutes") {
                config.idleUnloadMinutes = parseInt(value, 0);
            }
        }
    } else {
        hostLog(LISTERY_LOG_WARNING, "读取配置文件失败，使用默认设置");
    }

    std::sort(config.scan.excludeNames.begin(), config.scan.excludeNames.end());
    config.scan.excludeNames.erase(
        std::unique(config.scan.excludeNames.begin(), config.scan.excludeNames.end()),
        config.scan.excludeNames.end());
    std::sort(config.scan.excludePrefixes.begin(), config.scan.excludePrefixes.end());
    config.scan.excludePrefixes.erase(
        std::unique(config.scan.excludePrefixes.begin(), config.scan.excludePrefixes.end()),
        config.scan.excludePrefixes.end());

    if (config.rescanMinutes < 0)
        config.rescanMinutes = 0;
    if (config.rescanMinutes > 1440)
        config.rescanMinutes = 1440;
    if (config.idleUnloadMinutes > 1440)
        config.idleUnloadMinutes = 1440;

    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config = config;
    }
    g_rescanMinutes.store(config.rescanMinutes);
    g_matchPath.store(config.matchPath);
    g_idleUnloadMinutes.store(config.idleUnloadMinutes);
    return true;
}

// ---------------------------------------------------------------------------
// 索引快照与扫描线程
// ---------------------------------------------------------------------------

std::string indexSummary(const Index &index)
{
    std::string text = "索引就绪：";
    text += formatCount(index.files.size());
    text += " 个文件 / ";
    text += formatCount(index.dirs.size());
    text += " 个目录";
    if (index.scanMs > 0) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "，扫描 %.1f 秒",
                      static_cast<double>(index.scanMs) / 1000.0);
        text += buffer;
    }
    return text;
}

void publishIndex(Index &&index, bool haveSnapshot)
{
    auto shared = std::make_shared<Index>(std::move(index));
    const std::string summary = indexSummary(*shared);
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_index = shared;
        g_status = summary;
        g_haveSnapshot = haveSnapshot;
        g_builtAt = shared->builtAt;
    }
    hostLog(LISTERY_LOG_INFO, summary);
}

bool tryLoadSnapshot()
{
    setStatus("正在加载索引快照…");
    Index index;
    std::wstring error;
    if (!filesearch::loadIndex(g_indexPath, index, &error)) {
        if (!error.empty())
            hostLog(LISTERY_LOG_WARNING, filesearch::wideToUtf8(error));
        setStatus("正在建立索引（首次运行需要几分钟）…");
        return false;
    }
    publishIndex(std::move(index), true);
    return true;
}

void runScan()
{
    AppConfig config;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        config = g_config;
    }

    setStatus("正在扫描磁盘，请稍候…");
    hostLog(LISTERY_LOG_INFO, "开始扫描文件索引");

    const auto started = std::chrono::steady_clock::now();
    Index index;
    const auto scannerLog = [](const std::string &message) {
        setStatus(message);
    };
    const bool ok = filesearch::scanFileSystem(config.scan, index, g_stop, scannerLog);
    if (!ok || g_stop.load()) {
        setStatus("扫描已停止");
        hostLog(LISTERY_LOG_WARNING, "扫描已停止");
        return;
    }
    const auto finished = std::chrono::steady_clock::now();
    index.scanMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count());

    std::wstring error;
    bool saved = false;
    if (!filesearch::saveIndex(g_indexPath, index, &error)) {
        hostLog(LISTERY_LOG_WARNING,
                "保存索引快照失败：" + filesearch::wideToUtf8(error));
    } else {
        saved = true;
        hostLog(LISTERY_LOG_INFO, "索引快照已保存");
    }

    publishIndex(std::move(index), saved);
}

void scanLoop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    bool loaded = false;
    uint64_t lastAttempt = 0;

    while (!g_stop.load()) {
        if (!loaded) {
            tryLoadSnapshot();
            loaded = true;
        }

        const bool forced = g_forceScan.exchange(false);
        bool needScan = forced;
        bool unloaded = false;
        if (!needScan) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            if (!g_index) {
                // 有快照说明只是闲置卸载了，等下一次查询按需加载，不要主动重建。
                needScan = !g_haveSnapshot;
            } else {
                const int idleMinutes = g_idleUnloadMinutes.load();
                if (idleMinutes > 0 && g_haveSnapshot
                    && nowUnixSeconds() >= g_lastQueryAt.load()
                                               + static_cast<uint64_t>(idleMinutes) * 60) {
                    g_index.reset();
                    g_status = "索引已卸载：闲置超过 " + std::to_string(idleMinutes)
                               + " 分钟，下次搜索时自动加载";
                    unloaded = true;
                } else {
                    const int minutes = g_rescanMinutes.load();
                    if (minutes > 0
                        && nowUnixSeconds() >= g_builtAt + static_cast<uint64_t>(minutes) * 60)
                        needScan = true;
                }
            }
        }
        if (unloaded)
            hostLog(LISTERY_LOG_INFO, "索引已闲置卸载，下次搜索时从快照重新加载");

        const uint64_t now = nowUnixSeconds();
        if (needScan && (forced || lastAttempt == 0 || now - lastAttempt >= 60)) {
            lastAttempt = now;
            loadConfig();
            runScan();
        }

        for (int i = 0; i < 10 && !g_stop.load(); ++i)
            Sleep(100);
    }
}

// ---------------------------------------------------------------------------
// 检索线程
// ---------------------------------------------------------------------------

bool takeCancelled(unsigned long long id)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    return g_cancelled.erase(id) != 0;
}

void finishQuery(unsigned long long id, const char *error)
{
    if (g_host != nullptr && g_host->finish != nullptr)
        g_host->finish(g_host->context, id, error);
}

void addRow(unsigned long long id, const std::string &title, const std::string &subtitle,
            const std::string &badge, const std::string &action)
{
    if (g_host == nullptr || g_host->addResult == nullptr)
        return;
    g_host->addResult(g_host->context, id, title.c_str(),
                      subtitle.empty() ? nullptr : subtitle.c_str(),
                      badge.empty() ? nullptr : badge.c_str(),
                      action.empty() ? nullptr : action.c_str());
}

std::string fullFilePath(const Index &index, const FileEntry &file, const std::string &name)
{
    std::string path;
    filesearch::buildDirPath(index, file.dirId, path);
    if (!path.empty() && path.back() != '\\' && path.back() != '/')
        path.push_back('\\');
    path += name;
    return path;
}

std::string badgeForName(const std::string &name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size())
        return "文";
    std::string ext = name.substr(dot + 1);
    if (ext.size() > 3)
        return "文";
    for (char &ch : ext) {
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 32);
    }
    return ext;
}

void handleCommand(const QueryRequest &request)
{
    const std::string command = request.text;
    if (command == "!status") {
        const std::string status = statusText();
        std::string subtitle = "索引文件：";
        subtitle += filesearch::wideToUtf8(g_indexPath);
        subtitle += "（Enter 复制状态）";
        addRow(request.id, status, subtitle, "搜", "copy:" + status);
        finishQuery(request.id, nullptr);
        return;
    }
    if (command == "!reindex") {
        g_forceScan.store(true);
        addRow(request.id, "已请求重新扫描磁盘",
               "后台线程会重新读取配置并重建索引，完成后即可搜索",
               "搜", "copy:!reindex");
        finishQuery(request.id, nullptr);
        return;
    }
    if (command == "!config") {
        std::wstring params = L"\"";
        params += g_configPath;
        params += L"\"";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        addRow(request.id, "已打开配置文件",
               filesearch::wideToUtf8(g_configPath) + "（改完输入 !reindex 生效）",
               "搜", "copy:" + filesearch::wideToUtf8(g_configPath));
        finishQuery(request.id, nullptr);
        return;
    }
    if (command == "!help") {
        addRow(request.id, "直接输入文件名开始搜索", "例如 chrome、报告、setup.exe", "搜",
               "copy:!help");
        addRow(request.id, "按扩展名筛选：ext:pdf、ext:jpg,png、*.docx", "可以和其他关键字组合",
               "搜", "copy:ext:pdf");
        addRow(request.id, "按目录筛选：path:项目", "例如 path:Downloads 报告", "搜",
               "copy:path:");
        addRow(request.id, "!status 查看索引状态", "显示文件数、扫描时间和索引位置", "搜",
               "copy:!status");
        addRow(request.id, "!reindex 重新扫描", "修改配置或文件变化很大时使用", "搜",
               "copy:!reindex");
        addRow(request.id, "!config 打开配置文件", "可设置扫描根目录、排除目录和刷新间隔",
               "搜", "copy:!config");
        finishQuery(request.id, nullptr);
        return;
    }

    addRow(request.id, "无法识别的命令：" + command,
           "可用命令：!status、!reindex、!config、!help", "搜", "copy:!help");
    finishQuery(request.id, nullptr);
}

void processQuery(const QueryRequest &request)
{
    if (takeCancelled(request.id)) {
        finishQuery(request.id, nullptr);
        return;
    }

    if (!request.text.empty() && request.text.front() == '!') {
        handleCommand(request);
        return;
    }

    std::shared_ptr<const Index> index;
    std::string status;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        index = g_index;
        status = g_status;
    }

    // 闲置卸载后第一次查询：从快照重新加载，而不是让用户等一次全盘扫描。
    if (!index) {
        bool haveSnapshot = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            haveSnapshot = g_haveSnapshot;
        }
        if (haveSnapshot && tryLoadSnapshot()) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            index = g_index;
            status = g_status;
        } else if (haveSnapshot) {
            // 快照损坏或版本不兼容：让扫描线程重建。
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_haveSnapshot = false;
            g_forceScan.store(true);
        }
    }

    if (!index) {
        const std::string message = "索引尚未就绪：" + status;
        finishQuery(request.id, message.c_str());
        return;
    }

    const ParsedQuery parsed = filesearch::parseQuery(request.text);
    if (parsed.empty()) {
        finishQuery(request.id, nullptr);
        return;
    }

    int limit = request.maxResults > 0 ? request.maxResults : kDefaultMaxResults;
    if (limit > static_cast<int>(kMaxResultsCap))
        limit = static_cast<int>(kMaxResultsCap);

    const std::vector<uint32_t> hits =
        filesearch::searchIndex(*index, parsed, limit, g_matchPath.load());

    if (takeCancelled(request.id)) {
        finishQuery(request.id, nullptr);
        return;
    }

    for (size_t i = 0; i < hits.size(); ++i) {
        if ((i & 0x0F) == 0 && takeCancelled(request.id)) {
            finishQuery(request.id, nullptr);
            return;
        }
        const FileEntry &file = index->files[hits[i]];
        std::string name;
        if (!filesearch::fileNameAt(*index, hits[i], name))
            continue;
        const std::string path = fullFilePath(*index, file, name);
        std::string subtitle = path;
        subtitle += "    ";
        subtitle += filesearch::formatSize(filesearch::fileSize(file));
        addRow(request.id, name, subtitle, badgeForName(name), "run:" + path);
    }
    finishQuery(request.id, nullptr);
}

void searchLoop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        QueryRequest request;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait(lock, [] { return g_queueStop || !g_queue.empty(); });
            if (g_queueStop)
                return;
            request = std::move(g_queue.front());
            g_queue.pop_front();
        }
        processQuery(request);
    }
}

// ---------------------------------------------------------------------------
// 自检 / 预览模式
// ---------------------------------------------------------------------------

bool isSelfTestProcess()
{
    const wchar_t *commandLine = GetCommandLineW();
    if (commandLine == nullptr)
        return false;
    static const wchar_t *kSwitches[] = {L"--selftest", L"--render-preview", L"--test-hotkey"};
    for (const wchar_t *item : kSwitches) {
        if (wcsstr(commandLine, item) != nullptr)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 插件接口
// ---------------------------------------------------------------------------

int LISTERY_PLUGIN_CALL pluginInit(const ListeryHostV1 *host)
{
    if (host == nullptr || host->apiVersion < LISTERY_PLUGIN_API_VERSION)
        return 1;
    if (host->addResult == nullptr || host->finish == nullptr)
        return 2;

    g_host = host;

    g_dataDir = dataDirectory();
    g_indexPath = g_dataDir + L"\\index.bin";
    g_configPath = g_dataDir + L"\\filesearch.ini";

    SHCreateDirectoryExW(nullptr, g_dataDir.c_str(), nullptr);
    writeDefaultConfigIfMissing();
    loadConfig();

    hostLog(LISTERY_LOG_INFO, "文件搜索插件已加载，配置：" + filesearch::wideToUtf8(g_configPath));
    hostLog(LISTERY_LOG_INFO, "索引文件：" + filesearch::wideToUtf8(g_indexPath));

    g_stop.store(false);
    g_queueStop = false;
    g_lastQueryAt.store(nowUnixSeconds());
    g_searchThread = std::thread(searchLoop);

    if (isSelfTestProcess()) {
        setStatus("自检模式：跳过后台索引");
        return 0;
    }

    g_scanThread = std::thread(scanLoop);
    return 0;
}

void LISTERY_PLUGIN_CALL pluginShutdown()
{
    g_stop.store(true);
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queueStop = true;
        g_queue.clear();
    }
    g_queueCv.notify_all();

    if (g_scanThread.joinable())
        g_scanThread.join();
    if (g_searchThread.joinable())
        g_searchThread.join();

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_index.reset();
    }
    g_host = nullptr;
}

int LISTERY_PLUGIN_CALL pluginBeginQuery(unsigned long long requestId, const char *query,
                                         int maxResults)
{
    if (g_host == nullptr)
        return 1;

    QueryRequest request;
    request.id = requestId;
    request.text = query != nullptr ? std::string(query) : std::string();
    request.maxResults = maxResults > 0 ? maxResults : kDefaultMaxResults;
    g_lastQueryAt.store(nowUnixSeconds());

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.push_back(std::move(request));
    }
    g_queueCv.notify_one();
    return 0;
}

void LISTERY_PLUGIN_CALL pluginCancelQuery(unsigned long long requestId)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_cancelled.insert(requestId);
}

int LISTERY_PLUGIN_CALL pluginConfigure(void *nativeParentWindow)
{
    (void)nativeParentWindow;
    std::wstring params = L"\"";
    params += g_configPath;
    params += L"\"";
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", L"notepad.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32 ? 0 : 1;
}

const ListeryPluginV1 kPlugin = {
    LISTERY_PLUGIN_API_VERSION,
    "com.listery.filesearch",
    "文件搜索",
    "1.0.0",
    "用 Alt+S 快速搜索本机文件；自建索引，不依赖 Everything。",
    "Alt+S",
    pluginInit,
    pluginShutdown,
    pluginBeginQuery,
    pluginCancelQuery,
    pluginConfigure};

} // namespace

extern "C" LISTERY_PLUGIN_EXPORT const ListeryPluginV1 *LISTERY_PLUGIN_CALL listery_plugin_v1(void)
{
    return &kPlugin;
}
