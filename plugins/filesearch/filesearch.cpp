/*
 * Listery 文件搜索插件（路线 B：自建紧凑索引）
 *
 *  - 后台线程扫描用户目录、Public 和非系统固定磁盘；
 *  - 内存索引按文件名排序，检索走“全名 → 前缀 → 子串 → 路径”；
 *  - 支持 ext: 扩展名、path: 目录、full: 完整路径、re: 正则等过滤条件；
 *  - 索引二进制快照保存在 %LOCALAPPDATA%\Listery\FileSearch\index.bin；
 *  - 结果区按 Everything 风格分列（名称/路径/大小/修改日期…），列由 columns= 选择；
 *  - 配置文件 filesearch.ini 与快照同目录，可用 !config 打开；
 *  - 面板里输入 !status / !reindex / !config / !sort / !columns / !scope 查看状态、重建索引或改设置。
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
/** 闲置多少分钟后释放内存索引；0 = 常驻内存。 */
constexpr int kDefaultIdleUnloadMinutes = 10;
/**
 * 排序模式：0=off 保持相关度排序，1=name，2=size，3=mtime（修改时间），4=ctime（创建时间），
 * 5=path（完整路径），6=dir（所在目录），7=ext（扩展名）。
 * 排序方向：-1=自动（名称升序，大小/时间降序），0=强制升序，1=强制降序。
 */
constexpr int kDefaultSortMode = 0;
/**
 * 宿主可以一次要走全部命中：面板固定高度 + 滚动条浏览。
 * 上限只是防止“*.jpg”这类极宽查询把内存撑爆，正常搜索远达不到。
 */
constexpr uint32_t kMaxResultsCap = 20000;

// ---------------------------------------------------------------------------
// 列视图（Everything 风格）：列定义、配置解析与提交给宿主的 spec
// ---------------------------------------------------------------------------

enum ColumnId
{
    ColName = 0,
    ColPath,
    ColDir,
    ColSize,
    ColMtime,
    ColCtime,
    ColExt
};

struct ColumnDef
{
    ColumnId id;
    const char *key;   // 配置里写的名字
    const char *label;  // 表头文字（UTF-8）
    int width;          // 0 = 弹性列，>0 = 固定像素
    char align;         // 'l' 左对齐 / 'r' 右对齐
};

const ColumnDef kColumnDefs[] = {
    {ColName, "name", "名称", 0, 'l'},
    {ColPath, "path", "路径", 0, 'l'},
    {ColDir, "dir", "所在目录", 0, 'l'},
    {ColSize, "size", "大小", 92, 'r'},
    {ColMtime, "mtime", "修改日期", 140, 'l'},
    {ColCtime, "ctime", "创建日期", 140, 'l'},
    {ColExt, "ext", "扩展名", 76, 'l'},
};

std::vector<ColumnId> defaultColumns()
{
    return {ColName, ColPath, ColSize, ColMtime};
}

const ColumnDef *columnDefById(ColumnId id)
{
    for (const ColumnDef &def : kColumnDefs) {
        if (def.id == id)
            return &def;
    }
    return nullptr;
}

std::string trimAscii(const std::string &text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'
                           || text[begin] == '\n'))
        ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'
                          || text[end - 1] == '\r' || text[end - 1] == '\n'))
        --end;
    return text.substr(begin, end - begin);
}

std::string lowerAscii(const std::string &text)
{
    std::string out = text;
    for (char &ch : out) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + 32);
    }
    return out;
}

/** 解析 columns= 配置；off / 空 = 经典两行视图。未知列写入 badKey 并忽略。 */
std::vector<ColumnId> parseColumns(const std::string &text, std::string *badKey)
{
    std::vector<ColumnId> out;
    const std::string lowered = lowerAscii(trimAscii(text));
    if (lowered.empty() || lowered == "off" || lowered == "none")
        return out;

    size_t begin = 0;
    while (begin < lowered.size()) {
        size_t end = begin;
        while (end < lowered.size() && lowered[end] != ',' && lowered[end] != ';'
               && lowered[end] != '|' && lowered[end] != ' ' && lowered[end] != '\t')
            ++end;
        const std::string word = lowered.substr(begin, end - begin);
        if (!word.empty()) {
            const ColumnDef *found = nullptr;
            for (const ColumnDef &def : kColumnDefs) {
                if (word == def.key) {
                    found = &def;
                    break;
                }
            }
            if (found != nullptr) {
                if (std::find(out.begin(), out.end(), found->id) == out.end())
                    out.push_back(found->id);
            } else if (badKey != nullptr) {
                *badKey = word;
            }
        }
        if (end >= lowered.size())
            break;
        begin = end + 1;
    }
    return out;
}

std::string columnsToConfig(const std::vector<ColumnId> &columns)
{
    if (columns.empty())
        return "off";
    std::string out;
    for (ColumnId id : columns) {
        const ColumnDef *def = columnDefById(id);
        if (def == nullptr)
            continue;
        if (!out.empty())
            out.push_back(',');
        out += def->key;
    }
    return out;
}

/**
 * SDK 约定的列定义：标题:宽度:对齐:排序键[:方向]，用 ASCII US(0x1F) 分隔。
 * 排序键让宿主知道哪一列可以点击；方向只写在当前排序列上，用来画箭头。
 */
int effectiveDirection(int mode, int desc);
const char *sortModeName(int mode);

std::string columnSpec(const std::vector<ColumnId> &columns, int sortMode, int sortDesc)
{
    const int activeMode = sortMode > 0 ? sortMode : 0;
    const std::string activeKey =
        activeMode > 0 ? std::string(sortModeName(activeMode)) : std::string();
    const int direction = effectiveDirection(activeMode, sortDesc);
    std::string out;
    for (ColumnId id : columns) {
        const ColumnDef *def = columnDefById(id);
        if (def == nullptr)
            continue;
        if (!out.empty())
            out.push_back('\x1F');
        out += def->label;
        out.push_back(':');
        out += std::to_string(def->width);
        out.push_back(':');
        out.push_back(def->align);
        out.push_back(':');
        out += def->key;
        if (!activeKey.empty() && activeKey == def->key) {
            out.push_back(':');
            out += direction == 1 ? "desc" : "asc";
        }
    }
    return out;
}

std::string columnLabels(const std::vector<ColumnId> &columns)
{
    std::string out;
    for (ColumnId id : columns) {
        const ColumnDef *def = columnDefById(id);
        if (def == nullptr)
            continue;
        if (!out.empty())
            out += "、";
        out += def->label;
    }
    return out.empty() ? "经典两行视图" : out;
}

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
std::atomic<int> g_idleUnloadMinutes{kDefaultIdleUnloadMinutes};
std::atomic<int> g_sortMode{kDefaultSortMode};
std::atomic<int> g_sortDesc{-1};
std::atomic<uint64_t> g_lastQueryAt{0};

/** 索引范围：documents = 用户目录/非系统盘（默认）；all = 所有固定磁盘 + 隐藏与系统文件。 */
enum class IndexScope
{
    Documents,
    All
};

struct AppConfig
{
    ScanConfig scan;
    int rescanMinutes = kDefaultRescanMinutes;
    int idleUnloadMinutes = kDefaultIdleUnloadMinutes;
    bool matchPath = true;
    int sortMode = kDefaultSortMode;
    int sortDesc = -1;
    std::vector<ColumnId> columns = defaultColumns();
    IndexScope scope = IndexScope::Documents;
};

std::mutex g_configMutex;
AppConfig g_config;

/**
 * 当前查询实际使用的列。只有检索线程读写（每次查询开头重置），不需要加锁；
 * 命令类结果（!status / !sort 等）保持为空，用经典两行样式。
 */
std::vector<ColumnId> g_activeColumns;

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
/** 正在处理中的请求（受 g_queueMutex 保护），用于判断取消是否还有必要登记。 */
unsigned long long g_currentRequest = 0;
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

/** 某模式的默认方向：0=升序，1=降序（名称/路径/目录/扩展名默认升序，大小/时间默认降序）。 */
int autoDirection(int mode)
{
    return (mode == 1 || mode == 5 || mode == 6 || mode == 7) ? 0 : 1;
}

/** 当前实际生效的方向（desc=1 / asc=0）；auto（-1）按模式默认展开。 */
int effectiveDirection(int mode, int desc)
{
    if (mode == 0)
        return -1;
    return desc < 0 ? autoDirection(mode) : desc;
}

const char *sortModeName(int mode)
{
    switch (mode) {
    case 1: return "name";
    case 2: return "size";
    case 3: return "mtime";
    case 4: return "ctime";
    case 5: return "path";
    case 6: return "dir";
    case 7: return "ext";
    default: return "off";
    }
}

const char *sortModeLabel(int mode)
{
    switch (mode) {
    case 1: return "名称";
    case 2: return "大小";
    case 3: return "修改时间";
    case 4: return "创建时间";
    case 5: return "路径";
    case 6: return "所在目录";
    case 7: return "扩展名";
    default: return "相关度";
    }
}

bool parseSortMode(const std::string &word, int &mode)
{
    if (word == "off") mode = 0;
    else if (word == "name") mode = 1;
    else if (word == "size") mode = 2;
    else if (word == "mtime") mode = 3;
    else if (word == "ctime") mode = 4;
    else if (word == "path") mode = 5;
    else if (word == "dir") mode = 6;
    else if (word == "ext") mode = 7;
    else return false;
    return true;
}

/** 列排序键 → 排序模式；与 kColumnDefs 的 key 一一对应。 */
bool sortModeForKey(const std::string &key, int &mode)
{
    return parseSortMode(key, mode);
}

/** Unix 秒 → 本地时间 "YYYY-MM-DD HH:MM"；0 或转换失败返回空串。只在检索线程调用。 */
std::string formatLocalTime(uint32_t unixSeconds)
{
    if (unixSeconds == 0)
        return std::string();
    std::time_t stamp = static_cast<std::time_t>(unixSeconds);
    std::tm time{};
#if defined(_MSC_VER)
    if (localtime_s(&time, &stamp) != 0)
        return std::string();
#else
    if (localtime_r(&stamp, &time) == nullptr)
        return std::string();
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &time) == 0)
        return std::string();
    return std::string(buffer);
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
        if (!ReadFile(file, &out[0] + offset, request, &read, nullptr) || read == 0) {
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
    text += "; include_hidden：是否索引隐藏/系统文件，0 或 1（index_scope=all 时自动为 1）\r\n";
    text += "; index_scope：documents = 用户目录 + Public + 非系统固定磁盘（默认）；\r\n";
    text += ";   all = 所有固定磁盘（含系统盘 C:\\）+ 隐藏/系统文件 + 不排除 Windows/Program Files/AppData\r\n";
    text += ";   注意 all 会扫到几十万系统文件，首次建库明显变慢，改完必须 !reindex\r\n";
    text += "; match_path：文件名没命中时是否继续匹配目录路径，0 或 1\r\n";
    text += "; rescan_minutes：每隔多少分钟后台重建索引，0 = 只使用现有快照\r\n";
    text += "; idle_unload_minutes：闲置多少分钟后释放内存索引，下次搜索时自动从快照加载；默认 10，0 = 常驻内存\r\n";
    text += "; sort_mode：搜索结果排序，off（相关度）/ name / size / mtime（修改时间）/ ctime（创建时间）/ path / dir / ext；也可在搜索框输入 !sort 设置\r\n";
    text += "; sort_desc：排序方向，auto（名称/路径/目录/扩展名升序，大小和时间降序）/ asc / desc\r\n";
    text += "; 列视图下也可以直接点击结果区表头排序（与 !sort 写的是同一份配置）\r\n";
    text += "; columns：结果区显示的列，逗号分隔；off = 回到经典两行视图\r\n";
    text += ";   可选：name（名称）path（路径）dir（所在目录）size（大小）\r\n";
    text += ";         mtime（修改日期）ctime（创建日期）ext（扩展名）\r\n";
    text += ";   也可用 !columns name,path,size,mtime 在搜索框里直接改\r\n";
    text += "; config_version：配置格式版本（用于默认值升级），请勿修改\r\n";
    text += "\r\n";
    text += "config_version=2\r\n";
    text += "roots=\r\n";
    text += "exclude_names=\r\n";
    text += "exclude_prefixes=\r\n";
    text += "include_hidden=0\r\n";
    text += "index_scope=documents\r\n";
    text += "match_path=1\r\n";
    text += "rescan_minutes=60\r\n";
    text += "idle_unload_minutes=10\r\n";
    text += "sort_mode=off\r\n";
    text += "sort_desc=auto\r\n";
    text += "columns=name,path,size,mtime\r\n";

    if (!writeFileBytes(g_configPath, text))
        hostLog(LISTERY_LOG_WARNING, "无法写入默认配置文件");
}

/** 所有本地固定磁盘；includeSystemDrive=false 时跳过系统盘（默认文档范围）。 */
std::vector<std::wstring> fixedDriveRoots(bool includeSystemDrive)
{
    std::vector<std::wstring> roots;
    std::vector<std::wstring> keys;

    wchar_t windowsDir[MAX_PATH] = {};
    wchar_t systemDrive = L'C';
    if (GetWindowsDirectoryW(windowsDir, MAX_PATH) > 0)
        systemDrive = static_cast<wchar_t>(std::towupper(windowsDir[0]));

    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((drives & (1u << i)) == 0)
            continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        if (!includeSystemDrive && letter == systemDrive)
            continue;
        wchar_t root[4] = {letter, L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) != DRIVE_FIXED)
            continue;
        addUniquePath(roots, keys, root);
    }
    return roots;
}

std::vector<std::wstring> defaultRoots()
{
    std::vector<std::wstring> roots;
    std::vector<std::wstring> keys;
    addUniquePath(roots, keys, getEnv(L"USERPROFILE"));
    addUniquePath(roots, keys, getEnv(L"PUBLIC"));
    for (const std::wstring &root : fixedDriveRoots(false))
        addUniquePath(roots, keys, root);
    return roots;
}

/** 全盘范围下仍然排除的目录：系统保留区，遍历只会得到访问拒绝和垃圾数据。 */
std::vector<std::wstring> safetyExcludeNames()
{
    static const wchar_t *kNames[] = {L"$recycle.bin", L"system volume information",
                                     L"$windows.~bt", L"$windows.~ws"};
    std::vector<std::wstring> out;
    for (const wchar_t *name : kNames)
        out.push_back(lowerWide(name));
    return out;
}

const char *scopeName(IndexScope scope)
{
    return scope == IndexScope::All ? "all" : "documents";
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

/**
 * 替换 ini 中某个键的值（保留注释与其它键）；键不存在时插到文件开头。
 *
 * 旧实现有两个坑，这里一并修掉：
 *  1) 首行若带 UTF-8 BOM，键名比对时 BOM 被算进 key，导致之后每次写入都
 *     匹配不到、只能在 BOM 后再插一行 —— 用户的 ini 里会出现多条 sort_desc。
 *  2) 已经产生的重复键，写值时只改第一条、其余原样留着，读配置时最后一条
 *     反而生效。现在保留第一条、删掉其余重复行。
 */
void replaceIniValue(std::string &text, const std::string &key, const std::string &value)
{
    const bool hadBom = text.size() >= 3
                        && static_cast<unsigned char>(text[0]) == 0xEF
                        && static_cast<unsigned char>(text[1]) == 0xBB
                        && static_cast<unsigned char>(text[2]) == 0xBF;
    if (hadBom)
        text.erase(0, 3); // 先摘掉 BOM，末尾统一加回，键名比对才干净

    std::string out;
    out.reserve(text.size() + key.size() + value.size() + 4);
    bool replaced = false;
    size_t lineStart = 0;
    while (lineStart < text.size()) {
        const size_t lineEnd = text.find('\n', lineStart);
        const size_t fullEnd = lineEnd == std::string::npos ? text.size() : lineEnd + 1;
        const size_t contentEnd = lineEnd == std::string::npos ? text.size() : lineEnd;

        size_t pos = lineStart;
        while (pos < contentEnd && (text[pos] == ' ' || text[pos] == '\t'))
            ++pos;
        bool isKey = false;
        if (pos < contentEnd && text[pos] != ';' && text[pos] != '#') {
            size_t eq = pos;
            while (eq < contentEnd && text[eq] != '=')
                ++eq;
            if (eq < contentEnd) {
                size_t keyEnd = eq;
                while (keyEnd > pos && (text[keyEnd - 1] == ' ' || text[keyEnd - 1] == '\t'))
                    --keyEnd;
                isKey = text.compare(pos, keyEnd - pos, key) == 0;
            }
        }

        if (isKey) {
            if (!replaced) {
                const char *eol = "\r\n";
                if (lineEnd != std::string::npos) {
                    eol = (lineEnd > lineStart && text[lineEnd - 1] == '\r') ? "\r\n" : "\n";
                }
                out += key;
                out += "=";
                out += value;
                out += eol;
                replaced = true;
            }
            // 后续重复行直接丢弃
        } else {
            out.append(text, lineStart, fullEnd - lineStart);
        }
        lineStart = fullEnd;
    }

    if (!replaced)
        out.insert(0, key + "=" + value + "\r\n");
    if (hadBom)
        out.insert(0, "\xEF\xBB\xBF");
    text = std::move(out);
}

/** 旧配置迁移完成后回写，否则用户之后显式写的 0 会再次被迁移逻辑覆盖。 */
void persistConfigMigration(int idleUnloadMinutes)
{
    std::string raw;
    if (!readFileBytes(g_configPath, raw))
        return;
    replaceIniValue(raw, "config_version", "2");
    if (idleUnloadMinutes >= 0)
        replaceIniValue(raw, "idle_unload_minutes", std::to_string(idleUnloadMinutes));
    if (!writeFileBytes(g_configPath, raw))
        hostLog(LISTERY_LOG_WARNING, "配置迁移回写失败，下次启动会重复迁移");
}

/** !sort 的选择写回 ini（保留注释），让设置跨重启保留。 */
std::mutex g_iniWriteMutex;

void saveSortConfig(int mode, int desc)
{
    // 表头点击在 GUI 线程、!sort 在检索线程：两个写者必须串行，
    // 否则读改写交错会把刚写进去的方向覆盖掉。
    std::lock_guard<std::mutex> lock(g_iniWriteMutex);
    std::string raw;
    if (!readFileBytes(g_configPath, raw))
        raw = "\xEF\xBB\xBF";
    replaceIniValue(raw, "sort_mode", sortModeName(mode));
    replaceIniValue(raw, "sort_desc", desc < 0 ? "auto" : (desc == 1 ? "desc" : "asc"));
    if (!writeFileBytes(g_configPath, raw))
        hostLog(LISTERY_LOG_WARNING, "排序设置写入配置文件失败");
}

/** !columns / !scope 的选择写回 ini。两者都只改自己那一个键。 */
void saveConfigValue(const std::string &key, const std::string &value)
{
    std::string raw;
    if (!readFileBytes(g_configPath, raw))
        raw = "\xEF\xBB\xBF";
    replaceIniValue(raw, key, value);
    if (!writeFileBytes(g_configPath, raw))
        hostLog(LISTERY_LOG_WARNING, "配置写入失败：" + key);
}

bool loadConfig()
{
    AppConfig config;
    config.scan.roots = defaultRoots();
    config.scan.excludeNames = defaultExcludeNames();
    config.scan.excludePrefixes = defaultExcludePrefixes();
    config.scan.includeHidden = false;
    config.rescanMinutes = kDefaultRescanMinutes;
    config.idleUnloadMinutes = kDefaultIdleUnloadMinutes;
    config.matchPath = true;
    int configVersion = 1;
    bool migrateConfig = false;
    bool rootsExplicit = false;
    bool includeHiddenExplicit = false;
    std::vector<std::wstring> userExcludeNames;
    std::vector<std::wstring> userExcludePrefixes;

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
                    rootsExplicit = true;
                }
            } else if (key == "exclude_names") {
                userExcludeNames = splitList(filesearch::utf8ToWide(value));
            } else if (key == "exclude_prefixes") {
                userExcludePrefixes = splitList(filesearch::utf8ToWide(value));
            } else if (key == "include_hidden") {
                config.scan.includeHidden = parseBool(value, false);
                includeHiddenExplicit = true;
            } else if (key == "index_scope") {
                const std::string lowered = lowerAscii(value);
                if (lowered == "all")
                    config.scope = IndexScope::All;
                else if (lowered == "documents")
                    config.scope = IndexScope::Documents;
            } else if (key == "columns") {
                std::string badKey;
                config.columns = parseColumns(value, &badKey);
                if (!badKey.empty())
                    hostLog(LISTERY_LOG_WARNING, "columns 里未知的列已忽略：" + badKey);
            } else if (key == "match_path") {
                config.matchPath = parseBool(value, true);
            } else if (key == "rescan_minutes") {
                config.rescanMinutes = parseInt(value, kDefaultRescanMinutes);
            } else if (key == "idle_unload_minutes") {
                config.idleUnloadMinutes = parseInt(value, kDefaultIdleUnloadMinutes);
            } else if (key == "sort_mode") {
                int mode = kDefaultSortMode;
                if (parseSortMode(value, mode))
                    config.sortMode = mode;
            } else if (key == "sort_desc") {
                if (value == "asc")
                    config.sortDesc = 0;
                else if (value == "desc")
                    config.sortDesc = 1;
            } else if (key == "config_version") {
                configVersion = parseInt(value, 1);
            }
        }

        // 1.4.x 生成的默认文件把 idle_unload_minutes 写成 0（常驻）。没有版本标记时
        // 视为旧默认值，迁移到新的 10 分钟；带上 config_version=2 后 0 才是用户选择。
        if (configVersion < 2) {
            migrateConfig = true;
            if (config.idleUnloadMinutes == 0)
                config.idleUnloadMinutes = kDefaultIdleUnloadMinutes;
        }
    } else {
        hostLog(LISTERY_LOG_WARNING, "读取配置文件失败，使用默认设置");
    }

    // 迁移必须落盘，否则用户升级后显式改回 0 仍会被当成旧默认值。
    if (migrateConfig)
        persistConfigMigration(config.idleUnloadMinutes);

    // 全盘范围：根目录覆盖所有固定磁盘（含系统盘），去掉内置的 Windows/Program Files/
    // AppData 等排除项，并默认把隐藏/系统文件一起收进来。用户显式写的 roots 与
    // include_hidden 仍然优先，方便精确控制。
    if (config.scope == IndexScope::All) {
        if (!rootsExplicit)
            config.scan.roots = fixedDriveRoots(true);
        config.scan.excludeNames = safetyExcludeNames();
        config.scan.excludePrefixes.clear();
        if (!g_dataDir.empty())
            config.scan.excludePrefixes.push_back(lowerWide(normalizePath(g_dataDir)));
        if (!includeHiddenExplicit)
            config.scan.includeHidden = true;
    }
    appendExcludeNames(config.scan.excludeNames, userExcludeNames);
    for (const std::wstring &prefix : userExcludePrefixes)
        config.scan.excludePrefixes.push_back(lowerWide(prefix));

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
    g_sortMode.store(config.sortMode);
    g_sortDesc.store(config.sortDesc);
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
        bool rescan = false;
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
                        && nowUnixSeconds() >= g_builtAt + static_cast<uint64_t>(minutes) * 60) {
                        needScan = true;
                        rescan = true;
                    }
                }
            }
        }
        if (unloaded) {
            // 不裁剪整个进程工作集：那会把主程序页面也赶出去，下次操作反而卡顿。
            hostLog(LISTERY_LOG_INFO, "索引已闲置卸载，下次搜索时从快照重新加载");
        }

        const uint64_t now = nowUnixSeconds();
        const bool recentlyActive =
            now >= g_lastQueryAt.load() && now - g_lastQueryAt.load() < 60;
        if (needScan && !forced && rescan && recentlyActive) {
            // 用户正在使用搜索：周期重扫让路，等空闲后再扫。
            // 首次建库不受影响，否则启动后一分钟内搜不到任何文件。
        } else if (needScan && (forced || lastAttempt == 0 || now - lastAttempt >= 60)) {
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
            const std::string &badge, const std::string &action,
            const std::string &cells = std::string())
{
    if (g_host == nullptr)
        return;
    // 老宿主没有列视图扩展：安静地退回经典两行样式，不需要用户改配置。
    if (!g_activeColumns.empty() && g_host->addResultRow != nullptr) {
        g_host->addResultRow(g_host->context, id, title.c_str(),
                             subtitle.empty() ? nullptr : subtitle.c_str(),
                             badge.empty() ? nullptr : badge.c_str(),
                             action.empty() ? nullptr : action.c_str(),
                             cells.empty() ? nullptr : cells.c_str());
        return;
    }
    if (g_host->addResult == nullptr)
        return;
    g_host->addResult(g_host->context, id, title.c_str(),
                      subtitle.empty() ? nullptr : subtitle.c_str(),
                      badge.empty() ? nullptr : badge.c_str(),
                      action.empty() ? nullptr : action.c_str());
}

std::string directoryOf(const std::string &path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string extensionOf(const std::string &name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size())
        return "-";
    std::string ext = name.substr(dot + 1);
    for (char &ch : ext) {
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 32);
    }
    return ext;
}

/** 按配置顺序把一条命中拼成 '\x1F' 分隔的单元格文本。 */
std::string buildCells(const std::vector<ColumnId> &columns, const std::string &name,
                        const std::string &path, const FileEntry &file)
{
    std::string out;
    for (ColumnId id : columns) {
        if (!out.empty())
            out.push_back('\x1F');
        switch (id) {
        case ColName:
            out += name;
            break;
        case ColPath:
            out += path;
            break;
        case ColDir:
            out += directoryOf(path); // 按需算，没选这列就不切字符串
            break;
        case ColSize:
            out += filesearch::formatSize(filesearch::fileSize(file));
            break;
        case ColMtime: {
            const std::string time = formatLocalTime(file.mtime);
            out += time.empty() ? "-" : time;
            break;
        }
        case ColCtime: {
            const std::string time = formatLocalTime(file.ctime);
            out += time.empty() ? "-" : time;
            break;
        }
        case ColExt:
            out += extensionOf(name);
            break;
        default:
            break;
        }
    }
    return out;
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

/** 检索线程读配置：统一在锁内拷一份，避免跨线程直接访问 g_config。 */
AppConfig configSnapshot()
{
    std::lock_guard<std::mutex> lock(g_configMutex);
    return g_config;
}

void handleCommand(const QueryRequest &request)
{
    const std::string command = request.text;
    if (command == "!status") {
        const std::string status = statusText();
        const AppConfig config = configSnapshot();
        std::string subtitle = "范围：";
        subtitle += config.scope == IndexScope::All ? "all（全部固定磁盘）"
                                                    : "documents（用户目录 + 非系统盘）";
        subtitle += " · 隐藏/系统文件：";
        subtitle += config.scan.includeHidden ? "是" : "否";
        subtitle += " · 列：";
        subtitle += columnLabels(config.columns);
        subtitle += " · 索引：";
        subtitle += filesearch::wideToUtf8(g_indexPath);
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
        addRow(request.id, "按目录筛选：path:项目", "例如 path:Downloads 报告；/ 与 \\ 等价", "搜",
               "copy:path:");
        addRow(request.id, "按完整路径搜索：直接粘贴路径",
               "例如 C:\\Users\\me\\report.docx；支持通配符，如 D:\\*测试*；/ 与 \\ 等价", "搜",
               "copy:full:");
        addRow(request.id, "正则筛选：re:^IMG_.*\\.jpg$",
               "不区分大小写的文件名正则，可和其他条件组合；\\d、\\w 等转义保持原样", "搜",
               "copy:re:");
        addRow(request.id, "!sort 排序搜索结果",
               "!sort name/size/mtime/ctime，方向 asc/desc；立即生效并记住设置", "搜",
               "copy:!sort");
        addRow(request.id, "!columns 选择结果列",
               "!columns name,path,size,mtime；可选 dir/ctime/ext，off 回到经典两行视图", "搜",
               "copy:!columns");
        addRow(request.id, "!scope 切换索引范围",
               "!scope all 索引全部固定磁盘含隐藏/系统文件；!scope documents 回到默认", "搜",
               "copy:!scope");
        addRow(request.id, "!status 查看索引状态", "显示文件数、索引范围与当前列", "搜",
               "copy:!status");
        addRow(request.id, "!reindex 重新扫描", "修改配置或文件变化很大时使用", "搜",
               "copy:!reindex");
        addRow(request.id, "!config 打开配置文件", "可设置扫描根目录、排除目录和刷新间隔",
               "搜", "copy:!config");
        finishQuery(request.id, nullptr);
        return;
    }

    if (command == "!columns" || command.compare(0, 9, "!columns ") == 0) {
        const std::string arg =
            command.size() > 8 ? trimAscii(command.substr(8)) : std::string();
        const std::string choices =
            "可选：name path dir size mtime ctime ext；off = 经典两行视图";
        if (arg.empty()) {
            addRow(request.id, "当前列：" + columnLabels(configSnapshot().columns),
                   choices + "；例：!columns name,dir,size,mtime", "搜", "copy:!columns");
            finishQuery(request.id, nullptr);
            return;
        }
        std::string badKey;
        const std::vector<ColumnId> columns = parseColumns(arg, &badKey);
        if (!badKey.empty()) {
            addRow(request.id, "未知的列名：" + badKey, choices, "搜", "copy:!columns");
            finishQuery(request.id, nullptr);
            return;
        }
        saveConfigValue("columns", columnsToConfig(columns));
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            g_config.columns = columns;
        }
        addRow(request.id,
               columns.empty() ? std::string("已切回经典两行视图")
                               : "已切换列：" + columnLabels(columns),
               "设置已写入配置文件，立即生效", "搜", "copy:!columns");
        finishQuery(request.id, nullptr);
        return;
    }

    if (command == "!scope" || command.compare(0, 7, "!scope ") == 0) {
        const std::string arg = command.size() > 6 ? trimAscii(command.substr(6)) : std::string();
        const AppConfig current = configSnapshot();
        if (arg.empty()) {
            addRow(request.id, std::string("当前索引范围：") + scopeName(current.scope),
                   "documents = 用户目录/非系统盘且跳过隐藏文件；all = 全部固定磁盘含隐藏/系统文件",
                   "搜", "copy:!scope");
            finishQuery(request.id, nullptr);
            return;
        }
        const std::string lowered = lowerAscii(arg);
        if (lowered != "all" && lowered != "documents") {
            addRow(request.id, "未知的索引范围：" + arg,
                   "用法：!scope all 或 !scope documents", "搜", "copy:!scope");
            finishQuery(request.id, nullptr);
            return;
        }
        const IndexScope scope = lowered == "all" ? IndexScope::All : IndexScope::Documents;
        saveConfigValue("index_scope", scopeName(scope));
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            g_config.scope = scope;
        }
        // 扫描范围变了，旧索引里的条目集合不再成立，必须重建。
        g_forceScan.store(true);
        addRow(request.id, std::string("索引范围已切换为 ") + scopeName(scope),
               "后台正在重新读取配置并重建索引，可用 !status 查看进度", "搜",
               "copy:!scope");
        finishQuery(request.id, nullptr);
        return;
    }

    if (command == "!sort" || command.compare(0, 6, "!sort ") == 0) {
        // 解析 “!sort size desc” 这类参数：第一个关键字是模式，asc/desc 是方向。
        int mode = -1;
        int desc = -1;
        std::string badArg;
        size_t pos = 5;
        while (pos < command.size()) {
            while (pos < command.size() && (command[pos] == ' ' || command[pos] == '\t'))
                ++pos;
            const size_t end = command.find(' ', pos);
            const std::string word =
                command.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? command.size() : end;
            if (word.empty())
                break;
            int parsedMode = 0;
            if (parseSortMode(word, parsedMode))
                mode = parsedMode;
            else if (word == "asc")
                desc = 0;
            else if (word == "desc")
                desc = 1;
            else
                badArg = word;
        }

        const int currentMode = g_sortMode.load();
        const int currentDesc = g_sortDesc.load();
        if (!badArg.empty()) {
            addRow(request.id, "未知的排序选项：" + badArg,
                   "用法：!sort off/name/size/mtime/ctime/path/dir/ext [asc|desc]，单独输入 !sort 查看", "搜",
                   "copy:!sort");
            finishQuery(request.id, nullptr);
            return;
        }
        int newMode = mode;
        int newDesc = desc;
        if (newMode < 0 && newDesc < 0) {
            // 无参数：沿用当前模式并翻转方向；当前是 off 则切到名称升序。
            newMode = currentMode == 0 ? 1 : currentMode;
            newDesc = effectiveDirection(currentMode, currentDesc) == 1 ? 0 : 1;
        } else if (newMode < 0) {
            newMode = currentMode == 0 ? 1 : currentMode;
        } else if (newDesc < 0) {
            // 指定了模式没指定方向：同模式再次输入则翻转方向，换模式用该模式的默认方向。
            newDesc = newMode == currentMode
                          ? (effectiveDirection(currentMode, currentDesc) == 1 ? 0 : 1)
                          : autoDirection(newMode);
        }
        if (newMode == 0)
            newDesc = -1;
        g_sortMode.store(newMode);
        g_sortDesc.store(newDesc);
        saveSortConfig(newMode, newDesc);
        const std::string detail = newMode == 0
            ? std::string("结果恢复按匹配程度排序")
            : std::string(newDesc == 0 ? "升序" : newDesc == 1 ? "降序" : "默认方向");
        addRow(request.id, std::string("已切换为按") + sortModeLabel(newMode) + "排序",
               detail + "；设置已写入配置文件，立即生效", "搜", "copy:!sort");
        finishQuery(request.id, nullptr);
        return;
    }

    addRow(request.id, "无法识别的命令：" + command,
           "可用命令：!status、!reindex、!config、!sort、!columns、!scope、!help", "搜",
           "copy:!help");
    finishQuery(request.id, nullptr);
}

void processQuery(const QueryRequest &request)
{
    // 每次查询先复位：命令类结果与老宿主都走经典两行样式。
    g_activeColumns.clear();
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
    if (!parsed.regexError.empty()) {
        finishQuery(request.id, parsed.regexError.c_str());
        return;
    }
    if (parsed.empty()) {
        finishQuery(request.id, nullptr);
        return;
    }

    // 排序模式和列定义一起决定表头箭头：先读一次，随后整段查询都用这个值。
    const int sortMode = g_sortMode.load();
    const int sortDesc = g_sortDesc.load();

    // 声明列视图：必须在提交任何一行之前。老宿主没有这两个扩展指针时静默降级。
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_activeColumns = g_config.columns;
    }
    if (!g_activeColumns.empty() && g_host != nullptr
        && g_host->setResultColumns != nullptr && g_host->addResultRow != nullptr) {
        const std::string spec = columnSpec(g_activeColumns, sortMode, sortDesc);
        g_host->setResultColumns(g_host->context, request.id, spec.c_str());
    } else {
        g_activeColumns.clear();
    }

    int limit = request.maxResults > 0 ? request.maxResults : kDefaultMaxResults;
    if (limit > static_cast<int>(kMaxResultsCap))
        limit = static_cast<int>(kMaxResultsCap);

    // 排序模式下先拿回尽可能多的命中再重排：只排面板里那十几条没有意义。
    const bool sorted = sortMode != 0;
    const std::vector<uint32_t> found =
        filesearch::searchIndex(*index, parsed, sorted ? static_cast<int>(kMaxResultsCap) : limit,
                                g_matchPath.load(),
                                [&request]() { return takeCancelled(request.id); });

    if (takeCancelled(request.id)) {
        finishQuery(request.id, nullptr);
        return;
    }

    std::vector<uint32_t> hits = sorted ? [&] {
        std::vector<uint32_t> out(found.begin(), found.end());
        const int desc = effectiveDirection(sortMode, g_sortDesc.load());
        const bool descending = desc == 1;

        // 路径 / 所在目录 / 扩展名：先把比较键解码成字符串再排，
        // 避免比较函数里反复拼路径（2 万条命中时这是数量级的差别）。
        if (sortMode == 5 || sortMode == 6 || sortMode == 7) {
            std::vector<std::pair<uint32_t, std::string>> decorated;
            decorated.reserve(out.size());
            for (uint32_t fileIndex : out) {
                const FileEntry &file = index->files[fileIndex];
                std::string name;
                if (!filesearch::fileNameAt(*index, fileIndex, name))
                    continue;
                std::string key;
                if (sortMode == 5) {
                    key = fullFilePath(*index, file, name);
                } else if (sortMode == 6) {
                    filesearch::buildDirPath(*index, file.dirId, key);
                } else {
                    key = extensionOf(name);
                }
                decorated.emplace_back(fileIndex, lowerAscii(key));
            }
            std::sort(decorated.begin(), decorated.end(),
                      [descending](const std::pair<uint32_t, std::string> &a,
                                   const std::pair<uint32_t, std::string> &b) {
                          if (a.second != b.second)
                              return descending ? a.second > b.second : a.second < b.second;
                          return a.first < b.first;
                      });
            out.clear();
            out.reserve(decorated.size());
            for (const auto &item : decorated)
                out.push_back(item.first);
        } else {
            std::sort(out.begin(), out.end(),
                      [&index, sortMode, descending](uint32_t left, uint32_t right) {
                          const FileEntry &a = index->files[left];
                          const FileEntry &b = index->files[right];
                          uint64_t keyA = 0;
                          uint64_t keyB = 0;
                          switch (sortMode) {
                          case 1:
                              // files 数组本身就是文件名排序，比较下标等于比较名字，无需解码
                              return descending ? left > right : left < right;
                          case 2:
                              keyA = filesearch::fileSize(a);
                              keyB = filesearch::fileSize(b);
                              break;
                          case 3:
                              keyA = a.mtime;
                              keyB = b.mtime;
                              break;
                          case 4:
                              keyA = a.ctime;
                              keyB = b.ctime;
                              break;
                          default:
                              break;
                          }
                          if (keyA != keyB)
                              return descending ? keyA > keyB : keyA < keyB;
                          return left < right;
                      });
        }
        if (out.size() > static_cast<size_t>(limit))
            out.resize(static_cast<size_t>(limit));
        return out;
    }() : found;

    const bool showMtime = sortMode == 3;
    const bool showCtime = sortMode == 4;

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
        std::string cells;
        if (!g_activeColumns.empty()) {
            // 列视图下副标题只作 tooltip，细节都在单元格里，不必再拼一遍大小/时间。
            cells = buildCells(g_activeColumns, name, path, file);
        } else {
            subtitle += "    ";
            subtitle += filesearch::formatSize(filesearch::fileSize(file));
            if (showMtime || showCtime) {
                const std::string time = formatLocalTime(showMtime ? file.mtime : file.ctime);
                if (!time.empty()) {
                    subtitle += "    ";
                    subtitle += time;
                }
            }
        }
        addRow(request.id, name, subtitle, badgeForName(name), "run:" + path, cells);
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
            g_currentRequest = request.id;
        }
        processQuery(request);
        {
            // 处理完的请求不再需要取消标记：否则取消晚到时 set 会无限增长
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_cancelled.erase(request.id);
            g_currentRequest = 0;
        }
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
    // 只登记还在队列或正在处理的请求；已完成的请求永远等不到 takeCancelled
    if (g_currentRequest == requestId) {
        g_cancelled.insert(requestId);
        return;
    }
    for (const QueryRequest &queued : g_queue) {
        if (queued.id == requestId) {
            g_cancelled.insert(requestId);
            return;
        }
    }
}

int LISTERY_PLUGIN_CALL pluginSetResultSort(const char *columnKey, int ascending)
{
    if (columnKey == nullptr || *columnKey == '\0')
        return 1;
    int mode = 0;
    if (!sortModeForKey(std::string(columnKey), mode) || mode == 0)
        return 1;

    const int desc = ascending != 0 ? 0 : 1;
    g_sortMode.store(mode);
    g_sortDesc.store(desc);
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config.sortMode = mode;
        g_config.sortDesc = desc;
    }
    saveSortConfig(mode, desc);
    hostLog(LISTERY_LOG_INFO,
            std::string("表头排序：") + sortModeName(mode) + (desc == 1 ? " 降序" : " 升序"));
    return 0;
}

int LISTERY_PLUGIN_CALL pluginConfigure(void *nativeParentWindow)
{
    (void)nativeParentWindow;
    if (g_configPath.empty()) {
        // 插件被禁用时没有 init：打开设置仍要能找到（并在需要时生成）配置文件
        g_dataDir = dataDirectory();
        g_indexPath = g_dataDir + L"\\index.bin";
        g_configPath = g_dataDir + L"\\filesearch.ini";
        SHCreateDirectoryExW(nullptr, g_dataDir.c_str(), nullptr);
        writeDefaultConfigIfMissing();
        loadConfig();
    }
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
    "1.2.1",
    "用 Alt+S 快速搜索本机文件；Everything 风格的多列结果（列可配置、表头点击排序），自建索引，不依赖 Everything。",
    "Alt+S",
    pluginInit,
    pluginShutdown,
    pluginBeginQuery,
    pluginCancelQuery,
    pluginConfigure,
    pluginSetResultSort};

} // namespace

extern "C" LISTERY_PLUGIN_EXPORT const ListeryPluginV1 *LISTERY_PLUGIN_CALL listery_plugin_v1(void)
{
    return &kPlugin;
}
