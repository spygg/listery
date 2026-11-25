/*
 * filesearch_plugin_test：不启动 Listery 的插件端到端测试。
 *
 * 直接 LoadLibrary(filesearch.dll)，模拟宿主回调，扫描一个临时目录并查询，
 * 用来验证插件线程、快照和检索结果。仅开发使用。
 *
 * 用法：filesearch_plugin_test [filesearch.dll] [额外扫描根目录]
 */

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
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Row
{
    std::string title;
    std::string subtitle;
    std::string badge;
    std::string action;
    std::string cells; // 列视图下 '\x1f' 分隔的单元格
};

std::mutex g_mutex;
std::condition_variable g_cv;
unsigned long long g_waitId = 0;
bool g_finished = false;
std::string g_error;
std::vector<Row> g_rows;
/** 本次查询声明的列定义；空表示插件走了经典两行样式。 */
std::string g_columnsSpec;
const ListeryHostV1 *g_host = nullptr;

std::string fromWide(const std::wstring &text)
{
    if (text.empty())
        return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], needed,
                        nullptr, nullptr);
    return out;
}

std::wstring toWide(const std::string &text)
{
    if (text.empty())
        return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], needed);
    return out;
}

void LISTERY_PLUGIN_CALL hostAddResult(void *, unsigned long long requestId, const char *title,
                                       const char *subtitle, const char *badge,
                                       const char *action)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (requestId != g_waitId || g_finished)
        return;
    g_rows.push_back(Row{title ? title : "", subtitle ? subtitle : "", badge ? badge : "",
                         action ? action : "", ""});
}

void LISTERY_PLUGIN_CALL hostSetResultColumns(void *, unsigned long long requestId,
                                               const char *spec)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (requestId != g_waitId || g_finished)
        return;
    g_columnsSpec = spec ? spec : "";
}

void LISTERY_PLUGIN_CALL hostAddResultRow(void *, unsigned long long requestId, const char *title,
                                          const char *subtitle, const char *badge,
                                          const char *action, const char *cells)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (requestId != g_waitId || g_finished)
        return;
    Row row{title ? title : "", subtitle ? subtitle : "", badge ? badge : "",
           action ? action : "", cells ? cells : ""};
    g_rows.push_back(row);
}

void LISTERY_PLUGIN_CALL hostFinish(void *, unsigned long long requestId, const char *error)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (requestId != g_waitId)
        return;
    g_finished = true;
    g_error = error ? error : "";
    g_cv.notify_all();
}

void LISTERY_PLUGIN_CALL hostLog(void *, int level, const char *message)
{
    std::printf("[log %d] %s\n", level, message ? message : "");
}

bool query(const ListeryPluginV1 *plugin, const std::string &text, std::vector<Row> &rows,
           std::string &error, int maxResults = 12, int timeoutMs = 5000)
{
    unsigned long long id = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        static unsigned long long sequence = 1;
        id = sequence++;
        g_waitId = id;
        g_finished = false;
        g_error.clear();
        g_rows.clear();
        g_columnsSpec.clear();
    }

    if (plugin->beginQuery(id, text.c_str(), maxResults) != 0) {
        std::printf("beginQuery 拒绝：%s\n", text.c_str());
        return false;
    }

    std::unique_lock<std::mutex> lock(g_mutex);
    const bool ok = g_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  [] { return g_finished; });
    if (!ok) {
        std::printf("查询超时：%s\n", text.c_str());
        return false;
    }
    rows = g_rows;
    error = g_error;
    return true;
}

bool writeFile(const std::wstring &path, const std::string &content)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written,
                              nullptr)
                    && written == content.size();
    CloseHandle(file);
    return ok;
}

std::string readFile(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::string();
    std::string out;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        out.append(buffer, read);
    CloseHandle(file);
    return out;
}

/** 统计 ini 里某个键出现的行数（忽略注释行与 BOM）。 */
int countIniKey(const std::string &text, const std::string &key)
{
    int count = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos
                                                 ? std::string::npos
                                                 : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line.erase(0, 3);
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos)
            continue;
        const std::string trimmed = line.substr(first);
        if (trimmed.compare(0, key.size() + 1, key + "=") == 0)
            ++count;
    }
    return count;
}

std::vector<std::string> splitCells(const std::string &text)
{
    std::vector<std::string> out;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find('\x1F', begin);
        out.push_back(text.substr(begin, end == std::string::npos
                                                ? std::string::npos
                                                : end - begin));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return out;
}

std::wstring executableDirectory()
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH * 4);
    std::wstring text(path, length);
    const size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}

void printRows(const std::string &label, const std::vector<Row> &rows, const std::string &error)
{
    std::printf("%s：%zu 条", label.c_str(), rows.size());
    if (!error.empty())
        std::printf("（错误：%s）", error.c_str());
    std::printf("\n");
    for (size_t i = 0; i < rows.size() && i < 5; ++i) {
        std::printf("  %s | %s | %s\n", rows[i].title.c_str(), rows[i].subtitle.c_str(),
                    rows[i].action.c_str());
    }
}

} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr)
        return 1;

    std::wstring dll = executableDirectory() + L"\\plugins\\filesearch.dll";
    std::wstring extraRoot;
    if (argc >= 2 && argv[1][0] != L'-')
        dll = argv[1];
    if (argc >= 3)
        extraRoot = argv[2];
    LocalFree(argv);

    const DWORD processId = GetCurrentProcessId();
    wchar_t tempPath[MAX_PATH * 4] = {};
    GetTempPathW(MAX_PATH * 4, tempPath);
    const std::wstring base = std::wstring(tempPath) + L"filesearch_plugin_test_"
                              + std::to_wstring(processId);
    const std::wstring root = base + L"\\root";
    const std::wstring dataDir = base + L"\\AppData\\Local\\Listery\\FileSearch";
    SHCreateDirectoryExW(nullptr, (root + L"\\子目录").c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, dataDir.c_str(), nullptr);

    writeFile(root + L"\\测试报告 2026.txt", "hello");
    // 名字里“测试”在中间，且按文件名排序会排在“测试报告”前面；
    // 用来验证通配符查询会优先返回文件名开头命中的结果。
    writeFile(root + L"\\abc测试排序.txt", "order");
    // 结果数超过面板可见行数时，插件应把命中全部返回，由宿主的滚动条展示。
    for (int i = 0; i < 40; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "zzbulk-%02d.txt", i);
        writeFile(root + L"\\" + toWide(name), "bulk");
    }
    writeFile(root + L"\\chrome-settings.json", "{}");
    writeFile(root + L"\\sample.cpp", "int main(){}");
    writeFile(root + L"\\子目录\\嵌套文件.md", "# nested");
    // cases 表里有 appcontroller 用例；不建同名文件时它依赖本机真实目录，结果随机器漂移。
    writeFile(root + L"\\appcontroller.cs", "x");
    // 隐藏 + 系统属性文件：默认配置不该被索引，include_hidden=1 / index_scope=all 才收进来。
    const std::wstring hiddenFile = root + L"\\hidden_sys_probe.dat";
    writeFile(hiddenFile, "hidden");
    SetFileAttributesW(hiddenFile.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);

    // AppData 带隐藏属性、名字又在默认排除表里，从 USERPROFILE 往下遍历进不去。
    // 这里在沙盒里复刻「Furui\福瑞音乐」这类用户级应用数据目录，验证它作为显式
    // 补充根被单独索引（否则搜「福瑞音乐」一条都拿不到）。
    const std::wstring appDataDir = base + L"\\AppData\\Local\\Furui\\福瑞音乐";
    SHCreateDirectoryExW(nullptr, appDataDir.c_str(), nullptr);
    writeFile(appDataDir + L"\\probe_furui.txt", "probe");

    // 排序用例的固定数据：三个共享关键字 sortest 的 ASCII 文件名，
    // 名字顺序 a1 < b2 < c3，大小顺序 c3(17MB) > b2(200B) > a1(1B)。
    // 17MB 用来验证大文件排序与「大小」列的精确显示（旧版 24 位上限会显示 ≥16 MB）。
    writeFile(root + L"\\a1sortest.txt", std::string(1, 'x'));
    writeFile(root + L"\\b2sortest.txt", std::string(200, 'x'));
    writeFile(root + L"\\c3sortest.txt", std::string(17u * 1024u * 1024u, 'x'));
    // 显式设定三个不同的过去 mtime（a1 最早、c3 最新），不受本机时钟影响；
    // ctime 仍是创建时间（≈测试开始）。
    struct Stamp
    {
        const wchar_t *name;
        WORD year;
        WORD month;
        WORD day;
    };
    const Stamp stamps[] = {
        {L"\\a1sortest.txt", 2023, 3, 10},
        {L"\\b2sortest.txt", 2024, 7, 20},
        {L"\\c3sortest.txt", 2025, 11, 30},
    };
    for (const Stamp &stamp : stamps) {
        FILETIME asFileTime{};
        SYSTEMTIME when = {};
        when.wYear = stamp.year;
        when.wMonth = stamp.month;
        when.wDay = stamp.day;
        when.wHour = 12;
        when.wMinute = 34;
        when.wSecond = 21;
        if (SystemTimeToFileTime(&when, &asFileTime)) {
            HANDLE target = CreateFileW((root + stamp.name).c_str(), FILE_WRITE_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (target != INVALID_HANDLE_VALUE) {
                SetFileTime(target, nullptr, nullptr, &asFileTime);
                CloseHandle(target);
            }
        }
    }
    // 三个同大小文件让 name/size 的 tiebreak 可判定（名字顺序 a2 < b2 < c2）。
    for (wchar_t ch = L'a'; ch <= L'c'; ++ch)
        writeFile(root + L"\\" + std::wstring(1, ch) + L"2same.txt", std::string(50, 'x'));

    std::wstring roots = root;
    if (!extraRoot.empty())
        roots += L";" + extraRoot;

    std::string config = "roots=" + fromWide(roots) + "\r\n";
    config += "exclude_names=\r\n";
    config += "include_hidden=0\r\n";
    config += "index_scope=documents\r\n";
    config += "match_path=1\r\n";
    config += "rescan_minutes=0\r\n";
    config += "columns=name,path,size,mtime\r\n";
    writeFile(dataDir + L"\\filesearch.ini", config);

    std::printf("数据目录：%s\n", fromWide(dataDir).c_str());
    // 两个 AppData 环境都要指向沙盒：插件会把 Local/Roaming 作为显式补充根扫描，
    // 只改 LOCALAPPDATA 会把真实 Roaming 的数万文件带进来，污染排序等断言。
    const std::wstring sandboxLocal = base + L"\\AppData\\Local";
    const std::wstring sandboxRoaming = base + L"\\AppData\\Roaming";
    SHCreateDirectoryExW(nullptr, sandboxRoaming.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", sandboxLocal.c_str());
    SetEnvironmentVariableW(L"APPDATA", sandboxRoaming.c_str());

    HMODULE library = LoadLibraryW(dll.c_str());
    if (library == nullptr) {
        std::printf("加载插件失败：%s\n", fromWide(dll).c_str());
        return 1;
    }
    auto entry = reinterpret_cast<ListeryPluginEntry>(
        reinterpret_cast<void *>(GetProcAddress(library, "listery_plugin_v1")));
    if (entry == nullptr) {
        std::printf("找不到 listery_plugin_v1\n");
        return 1;
    }
    const ListeryPluginV1 *plugin = entry();
    if (plugin == nullptr) {
        std::printf("插件入口返回空\n");
        return 1;
    }

    ListeryHostV1 host{};
    host.apiVersion = LISTERY_PLUGIN_API_VERSION;
    host.addResult = hostAddResult;
    host.finish = hostFinish;
    host.log = hostLog;
    g_host = &host;

    if (plugin->init(&host) != 0) {
        std::printf("插件 init 失败\n");
        return 1;
    }
    std::printf("插件：%s %s，热键 %s\n", plugin->name, plugin->version ? plugin->version : "",
                plugin->defaultHotkey ? plugin->defaultHotkey : "");

    bool ready = false;
    std::vector<Row> rows;
    std::string error;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (query(plugin, "!status", rows, error) && !rows.empty()
            && rows.front().title.find("索引就绪") != std::string::npos) {
            ready = true;
            break;
        }
        Sleep(100);
    }
    printRows("!status", rows, error);
    if (!ready) {
        std::printf("索引没有在 20 秒内就绪\n");
        plugin->shutdown();
        return 1;
    }

    struct Case
    {
        const char *query;
        const char *expect;
    };
    const Case cases[] = {
        {"chrome", "chrome-settings.json"},
        {"测", "测试报告"},
        {"测试", "测试报告"},
        {"ext:cpp", ""},
        {"path:子目录", "子目录"},
        {"appcontroller", ""},
        // 隐藏的 AppData 里用户级应用目录必须能靠中文名命中（path 匹配）
        {"福瑞音乐", "福瑞音乐"},
    };

    int failures = 0;

    // 旧配置（没有 config_version）应被迁移并回写，否则 idle_unload_minutes=0 永远不生效
    {
        const std::string migrated = readFile(dataDir + L"\\filesearch.ini");
        const bool versionWritten = migrated.find("config_version=2") != std::string::npos;
        const bool idleWritten = migrated.find("idle_unload_minutes=10") != std::string::npos;
        std::printf("配置迁移回写：%s\n", versionWritten && idleWritten ? "通过" : "失败");
        if (!versionWritten || !idleWritten)
            ++failures;
    }

    // 取消（含已完成的 id）不应该影响后续查询或无限堆积
    plugin->beginQuery(999999, "chrome", 12);
    plugin->cancelQuery(999999);
    plugin->cancelQuery(1);

    for (const Case &item : cases) {
        if (!query(plugin, item.query, rows, error)) {
            ++failures;
            continue;
        }
        printRows(item.query, rows, error);
        if (rows.empty()) {
            ++failures;
            continue;
        }
        if (item.expect[0] != '\0'
            && rows.front().title.find(item.expect) == std::string::npos) {
            std::printf("  [FAIL] 期望首条包含 %s\n", item.expect);
            ++failures;
        }
        for (const Row &row : rows) {
            if (row.title.empty() || row.action.rfind("run:", 0) != 0) {
                std::printf("  [FAIL] 结果行缺少标题或 run 动作\n");
                ++failures;
                break;
            }
        }
    }

    // 正则 / 完整路径 / 错误提示
    struct ExtraCase
    {
        std::string queryText;
        const char *expect;
    };
    const std::string rootText = fromWide(root);
    const std::string fullPathQuery = rootText + "\\子目录\\嵌套文件.md";
    const ExtraCase extra[] = {
        {"re:^chrome.*json$", "chrome-settings.json"},
        {"re:^测试.*\\.txt", "测试报告"},
        {"re:^s[a-z]{4}e\\.cpp$", "sample.cpp"},
        {"re:^chrome-settings\\.(json|xml)$", "chrome-settings.json"},
        {"子目录/嵌套", "嵌套文件"},
        {rootText + "\\*嵌套*", "嵌套文件"},
        // 文件名开头命中优先于中间命中：abc测试排序.txt 排序在前，但首条应是“测试报告”
        {rootText + "\\*测试*", "测试报告"},
        // 裸通配符（不带路径分隔符）应直接匹配文件名，* 和扩展名都要生效
        {"*chrome*.json", "chrome-settings.json"},
        {"*嵌套*.md", "嵌套文件"},
        // Everything 风格：目录命中排在目录内容（文件）前面
        {"*子目录*", "子目录"},
        {"path:*子目录*", "子目录"},
        {fullPathQuery, "嵌套文件"},
        {"full:" + rootText + "\\子目录", "子目录"},
    };
    for (const ExtraCase &item : extra) {
        if (!query(plugin, item.queryText, rows, error)) {
            ++failures;
            continue;
        }
        printRows(item.queryText, rows, error);
        if (rows.empty() || rows.front().title.find(item.expect) == std::string::npos) {
            std::printf("  [FAIL] 期望首条包含 %s\n", item.expect);
            ++failures;
        }
    }

    // 文件夹本身也是命中：首行必须是目录（徽标/打开动作都指向目录），
    // 紧随其后才是目录里的文件，和 Everything 的顺序一致。
    {
        std::vector<Row> folderRows;
        std::string folderError;
        const bool issued = query(plugin, "*子目录*", folderRows, folderError);
        const std::string dirPath = fromWide(root + L"\\子目录");
        const bool folderFirst = issued && folderRows.size() >= 2
                                 && folderRows[0].title == "子目录"
                                 && folderRows[0].badge == "目录"
                                 && folderRows[0].action == "run:" + dirPath
                                 && folderRows[1].title.find("嵌套文件") != std::string::npos;
        std::printf("文件夹优先：%s（%zu 条）\n", folderFirst ? "通过" : "失败",
                    folderRows.size());
        if (!folderFirst)
            ++failures;
    }

    // 宿主要求一次取回全部命中：命中数超过面板可见行数时不能只给 12 条，
    // 面板拉高后剩下的靠滚动条浏览。
    {
        std::vector<Row> bulkRows;
        std::string bulkError;
        const bool issued = query(plugin, "zzbulk", bulkRows, bulkError, 1000);
        const bool allReturned = issued && bulkRows.size() >= 40;
        std::printf("一次取回全部命中：%s（%zu 条）\n",
                    allReturned ? "通过" : "失败", bulkRows.size());
        if (!allReturned)
            ++failures;
    }

    // 非法正则应该明确报错，而不是静默返回空结果
    if (query(plugin, "re:(", rows, error) && rows.empty() && !error.empty()) {
        std::printf("非法正则提示：通过（%s）\n", error.c_str());
    } else {
        std::printf("非法正则提示：失败\n");
        ++failures;
    }

    // 列视图扩展在排序测试之前挂上：表头箭头依赖 setResultColumns 收到的列定义。
    host.setResultColumns = hostSetResultColumns;
    host.addResultRow = hostAddResultRow;

    // !sort：模式 + 方向 + 持久化 + 时间显示
    {
        struct SortCase
        {
            const char *command;
            const char *queryText;
            const char *expectFirst;
            bool timeInSubtitle;
        };
        const SortCase sortCases[] = {
            {"!sort size", "sortest", "c3sortest.txt", false},
            {"!sort size asc", "sortest", "a1sortest.txt", false},
            {"!sort name desc", "sortest", "c3sortest.txt", false},
            {"!sort name", "sortest", "a1sortest.txt", false},
            // 同大小时按名字升序（tiebreak），验证排序稳定可预期
            {"!sort size", "same", "a2same.txt", false},
            {"!sort mtime desc", "sortest", "c3sortest.txt", true},
            // ctime 三个文件几乎相同，只验证时间列显示；首条不做断言（空串跳过检查）
            {"!sort ctime desc", "sortest", "", true},
            {"!sort off", "sortest", "a1sortest.txt", false},
        };
        for (const SortCase &item : sortCases) {
            std::vector<Row> setRows;
            std::string setError;
            if (!query(plugin, item.command, setRows, setError) || setRows.empty()) {
                std::printf("  [FAIL] %s 没有返回确认行\n", item.command);
                ++failures;
                continue;
            }
            if (setRows.front().title.find("已切换") == std::string::npos) {
                std::printf("  [FAIL] %s 确认行异常：%s\n", item.command,
                            setRows.front().title.c_str());
                ++failures;
            }
            if (!query(plugin, item.queryText, rows, error)) {
                ++failures;
                continue;
            }
            printRows(item.command, rows, error);
            if (rows.empty()
                || (item.expectFirst[0] != '\0' && rows.front().title != item.expectFirst)) {
                std::printf("  [FAIL] %s 后首条应为 %s，实际 %s\n", item.command,
                            item.expectFirst,
                            rows.empty() ? "(空)" : rows.front().title.c_str());
                ++failures;
            }
            const std::vector<std::string> sortCells =
                rows.empty() ? std::vector<std::string>() : splitCells(rows.front().cells);
            const bool hasTime = !rows.empty()
                                 && ((rows.front().subtitle.find("20") != std::string::npos
                                      && rows.front().subtitle.find('-') != std::string::npos)
                                     || (sortCells.size() >= 4
                                         && sortCells[3].find("20") != std::string::npos
                                         && sortCells[3].find('-') != std::string::npos));
            if (item.timeInSubtitle && !hasTime) {
                std::printf("  [FAIL] %s 副标题缺少时间：%s\n", item.command,
                            rows.front().subtitle.c_str());
                ++failures;
            }
        }

        const std::string iniAfter = readFile(dataDir + L"\\filesearch.ini");
        const bool persisted = iniAfter.find("sort_mode=off") != std::string::npos;
        std::printf("排序设置持久化：%s\n", persisted ? "通过" : "失败");
        if (!persisted)
            ++failures;

        // 旧版 replaceIniValue 首行带 BOM 时键名匹配永远失败，每改一次排序就
        // 往文件头插一行，sort_desc 会越堆越多。修复后每个键只能出现一次。
        const int sortDescs = countIniKey(iniAfter, "sort_desc");
        const int sortModes = countIniKey(iniAfter, "sort_mode");
        const bool deduped = sortDescs == 1 && sortModes == 1;
        std::printf("配置键去重：%s（sort_desc=%d，sort_mode=%d）\n",
                    deduped ? "通过" : "失败", sortDescs, sortModes);
        if (!deduped)
            ++failures;

        // 未知选项要报出来，不能静默改状态
        std::vector<Row> badRows;
        std::string badError;
        if (query(plugin, "!sort bogus", badRows, badError) && !badRows.empty()
            && badRows.front().title.find("未知") != std::string::npos) {
            std::printf("非法排序选项提示：通过\n");
        } else {
            std::printf("非法排序选项提示：失败\n");
            ++failures;
        }
        if (!query(plugin, "sortest", rows, error) || rows.empty()
            || rows.front().title != "a1sortest.txt") {
            std::printf("  [FAIL] 非法选项不应改变排序状态\n");
            ++failures;
        }
    }

    // ---- 隐藏/系统文件：默认配置（include_hidden=0）不该被索引 ----
    {
        std::vector<Row> hiddenRows;
        std::string hiddenError;
        if (query(plugin, "hidden_sys_probe", hiddenRows, hiddenError) && hiddenRows.empty()) {
            std::printf("默认跳过隐藏/系统文件：通过\n");
        } else {
            std::printf("默认跳过隐藏/系统文件：失败（%zu 条）\n", hiddenRows.size());
            ++failures;
        }
    }

    // ---- 表头点击排序：宿主通过 setResultSort 直接改排序并持久化 ----
    {
        bool ok = plugin->setResultSort != nullptr;
        std::printf("插件暴露 setResultSort：%s\n", ok ? "通过" : "失败");
        if (!ok)
            ++failures;

        if (ok) {
            std::vector<Row> sortRows;
            std::string sortError;
            const bool ascOk = plugin->setResultSort("size", 1) == 0
                && query(plugin, "sortest", sortRows, sortError) && !sortRows.empty()
                && sortRows.front().title == "a1sortest.txt"
                && g_columnsSpec.find("size:asc") != std::string::npos;
            std::printf("表头按大小升序：%s（首条=%s，列=%s）\n", ascOk ? "通过" : "失败",
                        sortRows.empty() ? "(空)" : sortRows.front().title.c_str(),
                        g_columnsSpec.c_str());
            if (!ascOk)
                ++failures;

            const std::string iniAsc = readFile(dataDir + L"\\filesearch.ini");
            const bool ascPersisted = iniAsc.find("sort_mode=size") != std::string::npos
                && iniAsc.find("sort_desc=asc") != std::string::npos;
            std::printf("表头排序持久化（升序）：%s\n", ascPersisted ? "通过" : "失败");
            if (!ascPersisted)
                ++failures;

            const bool descOk = plugin->setResultSort("size", 0) == 0
                && query(plugin, "sortest", sortRows, sortError) && !sortRows.empty()
                && sortRows.front().title == "c3sortest.txt"
                && g_columnsSpec.find("size:desc") != std::string::npos;
            std::printf("表头按大小降序（再点一次）：%s（首条=%s，列=%s）\n",
                        descOk ? "通过" : "失败",
                        sortRows.empty() ? "(空)" : sortRows.front().title.c_str(),
                        g_columnsSpec.c_str());
            if (!descOk)
                ++failures;

            const bool unknownRejected = plugin->setResultSort("bogus", 1) != 0;
            std::printf("未知列键拒绝排序：%s\n", unknownRejected ? "通过" : "失败");
            if (!unknownRejected)
                ++failures;

            for (const char *key : {"name", "size", "mtime", "ctime", "path", "dir", "ext"}) {
                if (plugin->setResultSort(key, 1) != 0) {
                    std::printf("  [FAIL] 合法列键被拒绝：%s\n", key);
                    ++failures;
                }
            }

            std::vector<Row> resetRows;
            std::string resetError;
            query(plugin, "!sort off", resetRows, resetError);
        }
    }

    // ---- 列视图：宿主两个扩展指针已在上方挂上 ----

    // 默认四列 + 精确大小（17MB 不该再显示 ≥16 MB）
    {
        std::vector<Row> colRows;
        std::string colError;
        if (!query(plugin, "c3sortest", colRows, colError) || colRows.empty()) {
            std::printf("列视图返回结果：失败\n");
            ++failures;
        } else {
            const std::string spec = g_columnsSpec;
            const size_t columnCount = spec.empty()
                ? 0
                : static_cast<size_t>(std::count(spec.begin(), spec.end(), '\x1F')) + 1;
            const std::vector<std::string> cells = splitCells(colRows.front().cells);
            const bool specOk = columnCount == 4
                && spec.find("名称:0:l") != std::string::npos
                && spec.find("路径:0:l") != std::string::npos
                && spec.find("大小:92:r") != std::string::npos
                && spec.find("修改日期:140:l") != std::string::npos;
            const bool cellsOk = cells.size() == 4 && cells[0] == "c3sortest.txt"
                && cells[2] == "17.0 MB" && cells[3].find("20") != std::string::npos;
            std::printf("默认列定义：%s（%zu 列）\n", specOk ? "通过" : "失败", columnCount);
            std::printf("单元格与精确大小：%s（大小=%s）\n", cellsOk ? "通过" : "失败",
                        cells.size() > 2 ? cells[2].c_str() : "(空)");
            if (!specOk || !cellsOk)
                ++failures;
        }
    }

    // 目录行在列视图里也要有完整单元格：没有的大小/时间显示 “-”
    {
        std::vector<Row> dirRows;
        std::string dirError;
        const bool issued = query(plugin, "*子目录*", dirRows, dirError);
        const std::vector<std::string> cells =
            dirRows.empty() ? std::vector<std::string>() : splitCells(dirRows.front().cells);
        const bool dirCellsOk = issued && !dirRows.empty()
                                && dirRows.front().title == "子目录" && cells.size() == 4
                                && cells[0] == "子目录" && cells[2] == "-" && cells[3] == "-";
        std::printf("目录行列单元格：%s\n", dirCellsOk ? "通过" : "失败");
        if (!dirCellsOk)
            ++failures;
    }

    // 配置里换列：!columns name,size 应该立刻生效并写入 ini
    {
        std::vector<Row> setRows;
        std::string setError;
        query(plugin, "!columns name,size", setRows, setError);
        std::vector<Row> colRows;
        std::string colError;
        const bool ok = query(plugin, "c3sortest", colRows, colError) && !colRows.empty()
            && g_columnsSpec.find("路径") == std::string::npos
            && splitCells(colRows.front().cells).size() == 2;
        std::printf("!columns 自定义列：%s\n", ok ? "通过" : "失败");
        if (!ok)
            ++failures;
    }

    // off 要能退回经典两行样式（老宿主路径）
    {
        std::vector<Row> setRows;
        std::string setError;
        query(plugin, "!columns off", setRows, setError);
        std::vector<Row> colRows;
        std::string colError;
        const bool ok = query(plugin, "c3sortest", colRows, colError) && !colRows.empty()
            && g_columnsSpec.empty() && colRows.front().cells.empty()
            && colRows.front().subtitle.find("17.0 MB") != std::string::npos;
        std::printf("!columns off 回退：%s\n", ok ? "通过" : "失败");
        if (!ok)
            ++failures;
    }

    // 非法列名要提示，不能静默吞掉
    {
        std::vector<Row> badRows;
        std::string badError;
        const bool ok = query(plugin, "!columns bogus", badRows, badError) && !badRows.empty()
            && badRows.front().title.find("未知") != std::string::npos;
        std::printf("非法列名提示：%s\n", ok ? "通过" : "失败");
        if (!ok)
            ++failures;
    }

    // !scope 只查询不改配置，不会触发全盘扫描
    {
        std::vector<Row> scopeRows;
        std::string scopeError;
        const bool ok = query(plugin, "!scope", scopeRows, scopeError) && !scopeRows.empty()
            && scopeRows.front().title.find("documents") != std::string::npos;
        std::printf("!scope 当前范围：%s\n", ok ? "通过" : "失败");
        if (!ok)
            ++failures;
    }

    // index_scope=all：显式 roots 保住测试目录，同时隐藏/系统文件应被收录
    {
        std::string config2 = "roots=" + fromWide(roots) + "\r\n";
        config2 += "exclude_names=\r\n";
        config2 += "exclude_prefixes=\r\n";
        config2 += "index_scope=all\r\n";
        config2 += "match_path=1\r\n";
        config2 += "rescan_minutes=0\r\n";
        config2 += "columns=name,path,size,mtime\r\n";
        writeFile(dataDir + L"\\filesearch.ini", config2);

        std::vector<Row> rows;
        std::string error;
        query(plugin, "!reindex", rows, error);
        bool found = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (query(plugin, "hidden_sys_probe", rows, error, 12, 1000) && !rows.empty()) {
                found = true;
                break;
            }
            Sleep(100);
        }
        std::printf("index_scope=all 收录隐藏/系统文件：%s\n", found ? "通过" : "失败");
        if (!found)
            ++failures;
    }

    plugin->shutdown();
    // 插件线程已在 shutdown 里 join，这里可以安全卸载。
    FreeLibrary(library);

    std::printf(failures == 0 ? "全部通过\n" : "失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
