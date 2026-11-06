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
};

std::mutex g_mutex;
std::condition_variable g_cv;
unsigned long long g_waitId = 0;
bool g_finished = false;
std::string g_error;
std::vector<Row> g_rows;
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
                         action ? action : ""});
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
           std::string &error, int timeoutMs = 5000)
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
    }

    if (plugin->beginQuery(id, text.c_str(), 12) != 0) {
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
    writeFile(root + L"\\chrome-settings.json", "{}");
    writeFile(root + L"\\sample.cpp", "int main(){}");
    writeFile(root + L"\\子目录\\嵌套文件.md", "# nested");

    std::wstring roots = root;
    if (!extraRoot.empty())
        roots += L";" + extraRoot;

    std::string config = "roots=" + fromWide(roots) + "\r\n";
    config += "exclude_names=\r\n";
    config += "include_hidden=0\r\n";
    config += "match_path=1\r\n";
    config += "rescan_minutes=0\r\n";
    writeFile(dataDir + L"\\filesearch.ini", config);

    std::printf("数据目录：%s\n", fromWide(dataDir).c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", (base + L"\\AppData\\Local").c_str());

    HMODULE library = LoadLibraryW(dll.c_str());
    if (library == nullptr) {
        std::printf("加载插件失败：%s\n", fromWide(dll).c_str());
        return 1;
    }
    auto entry = reinterpret_cast<ListeryPluginEntry>(
        GetProcAddress(library, "listery_plugin_v1"));
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
        {"path:子目录", "嵌套文件"},
        {"appcontroller", ""},
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
        {"re:测试.*\\.txt", "测试报告"},
        {"re:^s[a-z]{4}e\\.cpp$", "sample.cpp"},
        {"re:^chrome-settings\\.(json|xml)$", "chrome-settings.json"},
        {"子目录/嵌套", "嵌套文件"},
        {fullPathQuery, "嵌套文件"},
        {"full:" + rootText + "\\子目录", "嵌套文件"},
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

    // 非法正则应该明确报错，而不是静默返回空结果
    if (query(plugin, "re:(", rows, error) && rows.empty() && !error.empty()) {
        std::printf("非法正则提示：通过（%s）\n", error.c_str());
    } else {
        std::printf("非法正则提示：失败\n");
        ++failures;
    }

    plugin->shutdown();
    // 插件线程已在 shutdown 里 join，这里可以安全卸载。
    FreeLibrary(library);

    std::printf(failures == 0 ? "全部通过\n" : "失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
