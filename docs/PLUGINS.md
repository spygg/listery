# Listery 插件开发指南

Listery 的插件是**普通动态库**，接口是**纯 C 的 ABI**：不传 C++ 对象、不依赖 Qt、不需要 OpenSSL。
因此同一份插件二进制可以在 Qt 5.7（Windows XP 版）、Qt 5.15、Qt 6 的 Listery 上加载。

- 接口头：[`sdk/listery_plugin_api.h`](../sdk/listery_plugin_api.h)
- 参考实现：[`plugins/translator/`](../plugins/translator/)（英汉翻译，约 400 行，零依赖）
- 英文版：[PLUGINS_EN.md](PLUGINS_EN.md)

## 30 秒上手

1. 复制 `sdk/listery_plugin_api.h` 到你的工程；
2. 实现 `ListeryPluginV1` 并导出 `listery_plugin_v1`；
3. 编译成动态库，文件名随意（如 `translator.dll`），放进 Listery 的 `plugins/` 目录；
4. 重启 Listery —— 插件出现在「设置 → 插件」，热键与启用状态会自动写进 `settings.ini`。

## 目录与加载规则

| 项目 | 规则 |
| --- | --- |
| 插件目录 | 可执行文件同目录下的 `plugins/`（便携版就是解压目录；安装版是安装目录） |
| 识别后缀 | Windows `*.dll`、Linux `*.so`、macOS `*.dylib`（也接受 `*.so`） |
| 入口符号 | `listery_plugin_v1`（C 链接，返回 `const ListeryPluginV1 *`） |
| 版本校验 | `apiVersion` 必须等于 `LISTERY_PLUGIN_API_VERSION`，否则拒绝加载并在设置页显示原因 |
| 重复 id | 后加载的同 id 插件会被跳过 |
| 卸载 | 插件在进程生命周期内常驻，不支持热卸载；「禁用」只是不再调用它 |

## 接口

```c
typedef struct ListeryPluginV1 {
    int apiVersion;
    const char *id;             // 稳定标识，用于保存启用状态与热键，如 com.example.foo
    const char *name;           // 显示名
    const char *version;        // 版本字符串
    const char *description;    // 一句话说明
    const char *defaultHotkey;  // 默认全局热键，如 "Alt+E"；留空表示不占热键

    int  (*init)(const ListeryHostV1 *host);            // 返回 0 成功
    void (*shutdown)(void);
    int  (*beginQuery)(unsigned long long requestId, const char *query, int maxResults);
    void (*cancelQuery)(unsigned long long requestId);
    int  (*configure)(void *nativeParentWindow);        // 可选，可填 NULL
} ListeryPluginV1;
```

宿主能力表（插件在 `init()` 里保存指针，之后一直有效）：

```c
typedef struct ListeryHostV1 {
    int  apiVersion;
    void *context;                       // 调用下面每个函数时原样传回
    void (*addResult)(void *context, unsigned long long requestId,
                      const char *title, const char *subtitle,
                      const char *badge, const char *action);
    void (*finish)(void *context, unsigned long long requestId, const char *errorMessage);
    void (*log)(void *context, int level, const char *message);
    int  (*httpGet)(void *context, const char *url, const char *headers,
                    ListeryHttpCallback callback, void *userData, int timeoutMs);
} ListeryHostV1;
```

### 一次查询的时序

```
用户输入 → 宿主调用 beginQuery(requestId, query, maxResults)
           插件可以立刻返回结果，也可以先返回 0、稍后异步提交
异步完成 → addResult(...) 提交 0~N 行 → finish(requestId, NULL)
输入变了 / 面板关了 → cancelQuery(requestId)   （插件可忽略，但应尽快停止网络动作）
```

约定（务必遵守）：

- `query` 只在 `beginQuery` 调用期间有效，需要留存请自己拷贝；
- **每次请求必须调用一次 `finish`**（正常结束传 `NULL`，出错传错误信息），否则界面不会刷新；
- `addResult` / `finish` 可以从任意线程调用，宿主内部加锁并通过队列切回 GUI 线程；
- 连续输入时旧的 `requestId` 结果会被丢弃，插件不需要自己排序；
- `title` 建议控制在 120 字符内（UI 会截断显示）。

### action 协议

`addResult` 的 `action` 决定回车 / 双击这一行时做什么：

| 写法 | 行为 |
| --- | --- |
| `copy:文本` | 复制到剪贴板，并弹一条托盘提示 |
| `url:https://...` | 用系统默认浏览器打开 |
| `run:完整路径` | 用系统默认方式打开该路径 |
| `open:完整路径` | 在文件管理器里定位该文件 |
| 空 / NULL | 只收起面板，不执行动作 |

`badge` 是行首徽标，1~2 个字符最合适（翻译插件用「译」）。

### 网络请求

插件**不要**自己引 OpenSSL / libcurl：用宿主的 `httpGet`。

```c
static void onHttp(void *userData, int status, const char *body,
                   unsigned int size, const char *error) {
    /* 在宿主的网络线程（通常是主线程）回调，别在这里阻塞 */
}

g_host->httpGet(g_host->context, url,
                "User-Agent: my-plugin/1.0\nAccept: application/json",
                onHttp, userData, 6000);
```

- `headers` 用 `\n` 分隔多行，可传 `NULL`；
- `timeoutMs <= 0` 用宿主默认（8 秒）；超时会以 `error = "请求超时"` 回调；
- 优先用明文 HTTP 的公开接口（翻译插件就这么做），可以避免目标机缺 TLS 运行库。

### 全局热键

- 写法：`Alt+E`、`Ctrl+Shift+F1`；修饰键支持 `Ctrl/Control`、`Alt/Option`、`Shift`、`Meta/Win/Cmd/Command/Super`；
- 主键支持 `A–Z`、`0–9`、`F1–F12`、`Space`、`Enter`、`Tab`、`Esc`、`Home`、`End`、`Insert`、`Del`；
- 修饰键**严格匹配**：登记 `Alt+E` 后，`Ctrl+Alt+E` 不会触发；
- 用户可在「设置 → 插件」里改热键或清空（清空后只能从托盘的「插件」子菜单进入）。

## 最小插件

```cpp
#include "listery_plugin_api.h"
#include <string>

static const ListeryHostV1 *g_host = nullptr;

static int pluginInit(const ListeryHostV1 *host)
{
    if (host == nullptr || host->apiVersion < LISTERY_PLUGIN_API_VERSION)
        return 1;
    g_host = host;
    return 0;
}

static void pluginShutdown() { g_host = nullptr; }

static int pluginBeginQuery(unsigned long long requestId, const char *query, int)
{
    if (g_host == nullptr)
        return 1;
    const std::string echo = std::string("你说的是：") + (query ? query : "");
    g_host->addResult(g_host->context, requestId, echo.c_str(),
                      "示例插件", "例", ("copy:" + echo).c_str());
    g_host->finish(g_host->context, requestId, nullptr);
    return 0;
}

static void pluginCancelQuery(unsigned long long) {}

static const ListeryPluginV1 kPlugin = {
    LISTERY_PLUGIN_API_VERSION,
    "com.example.echo", "回音", "1.0.0", "把输入原样返回一遍",
    "Alt+E",                       // 默认热键
    pluginInit, pluginShutdown, pluginBeginQuery, pluginCancelQuery, nullptr
};

extern "C" LISTERY_PLUGIN_EXPORT const ListeryPluginV1 *LISTERY_PLUGIN_CALL
listery_plugin_v1(void) { return &kPlugin; }
```

配套的 CMake（照抄 `plugins/translator/CMakeLists.txt` 即可）：

```cmake
add_library(my_plugin MODULE my_plugin.cpp)
set_target_properties(my_plugin PROPERTIES
    CXX_STANDARD 14 CXX_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
if(WIN32)
    set_target_properties(my_plugin PROPERTIES PREFIX "" SUFFIX ".dll")
endif()
target_include_directories(my_plugin PRIVATE <sdk 目录>)
```

## 调试与排错

- 插件调用 `host->log` 的内容会写进 `listery.log`（级别：`LISTERY_LOG_DEBUG/INFO/WARNING`）；
- 加载失败（缺符号、apiVersion 不符、init 非 0）不会弹窗，原因显示在「设置 → 插件」里；
- 自检 `listery --selftest` 会加载 `plugins/` 目录并跑一次真实查询，可用于验证部署是否完整。

## 已知限制

- 目前只有**查询型插件**一种形态（`beginQuery`），暂不支持常驻面板、后台任务或自定义结果样式；
- 插件进程内常驻，不支持热更新（换文件后需重启 Listery）；
- `configure` 收到的是原生窗口句柄，插件需要自己画界面（不要依赖 Qt）；
- Linux 下 Wayland 会话拿不到全局按键，热键不可用（托盘菜单仍可进入插件）。
