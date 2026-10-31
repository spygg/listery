# Listery Plugin Guide

A Listery plugin is a **plain shared library** behind a **pure C ABI**: no C++ objects cross the
boundary, no Qt, no OpenSSL. The same binary therefore loads into the Qt 5.7 (Windows XP),
Qt 5.15 and Qt 6 builds of Listery.

- Header: [`sdk/listery_plugin_api.h`](../sdk/listery_plugin_api.h)
- Reference implementation: [`plugins/translator/`](../plugins/translator/) (EN↔ZH translator, ~400 lines, zero dependencies)
- 中文版：[PLUGINS.md](PLUGINS.md)

## 30-second start

1. Copy `sdk/listery_plugin_api.h` into your project;
2. Implement `ListeryPluginV1` and export `listery_plugin_v1`;
3. Build a shared library (any file name, e.g. `translator.dll`) and drop it into Listery's `plugins/` folder;
4. Restart Listery — the plugin shows up under Settings → Plugins; its hotkey and enabled state are stored in `settings.ini`.

## Discovery and loading

| Item | Rule |
| --- | --- |
| Folder | `plugins/` next to the executable (the extracted folder for the portable build, the install folder for the setup build) |
| Extensions | Windows `*.dll`, Linux `*.so`, macOS `*.dylib` (`*.so` accepted too) |
| Entry symbol | `listery_plugin_v1` (C linkage, returns `const ListeryPluginV1 *`) |
| Version check | `apiVersion` must equal `LISTERY_PLUGIN_API_VERSION`, otherwise the plugin is rejected and the reason is shown in Settings |
| Duplicate id | A second plugin with the same id is skipped |
| Unloading | Plugins stay loaded for the process lifetime; "disabled" only means they are no longer called |

## The interface

```c
typedef struct ListeryPluginV1 {
    int apiVersion;
    const char *id;             // stable id, used to persist enabled state and hotkey
    const char *name;
    const char *version;
    const char *description;
    const char *defaultHotkey;  // e.g. "Alt+E"; empty means "do not grab a hotkey"

    int  (*init)(const ListeryHostV1 *host);            // return 0 on success
    void (*shutdown)(void);
    int  (*beginQuery)(unsigned long long requestId, const char *query, int maxResults);
    void (*cancelQuery)(unsigned long long requestId);
    int  (*configure)(void *nativeParentWindow);        // optional, may be NULL
} ListeryPluginV1;
```

Host capability table (save the pointer during `init()`, it stays valid):

```c
typedef struct ListeryHostV1 {
    int  apiVersion;
    void *context;                       // pass back verbatim on every call
    void (*addResult)(void *context, unsigned long long requestId,
                      const char *title, const char *subtitle,
                      const char *badge, const char *action);
    void (*finish)(void *context, unsigned long long requestId, const char *errorMessage);
    void (*log)(void *context, int level, const char *message);
    int  (*httpGet)(void *context, const char *url, const char *headers,
                    ListeryHttpCallback callback, void *userData, int timeoutMs);
} ListeryHostV1;
```

### Query lifecycle

```
user types → host calls beginQuery(requestId, query, maxResults)
             you may answer immediately, or return 0 and submit rows later
async done → addResult(...) zero or more times → finish(requestId, NULL)
input changed / panel closed → cancelQuery(requestId)   (may be ignored, but stop network work)
```

Rules (please follow them):

- `query` is only valid during the `beginQuery` call — copy it if you need to keep it;
- **call `finish` exactly once per request** (pass `NULL` on success, a message on error), otherwise the panel never refreshes;
- `addResult` / `finish` may be called from any thread; the host locks internally and hops back to the GUI thread;
- while the user keeps typing, older `requestId` results are dropped — no ordering work needed on your side;
- keep `title` under ~120 characters (the UI elides longer text).

### action protocol

`action` decides what Enter / double click does:

| Value | Behaviour |
| --- | --- |
| `copy:text` | Copy to clipboard and show a tray notification |
| `url:https://...` | Open in the default browser |
| `run:full path` | Open the path with the system default handler |
| `open:full path` | Reveal the file in the file manager |
| empty / NULL | Just close the panel |

`badge` is the short label drawn at the left of the row (the translator uses 译).

### Networking

Do **not** pull in OpenSSL or libcurl — use the host's `httpGet`:

```c
static void onHttp(void *userData, int status, const char *body,
                   unsigned int size, const char *error) {
    /* invoked on the host's network thread (usually the main thread) — don't block here */
}

g_host->httpGet(g_host->context, url,
                "User-Agent: my-plugin/1.0\nAccept: application/json",
                onHttp, userData, 6000);
```

- `headers` is `\n`-separated and may be `NULL`;
- `timeoutMs <= 0` uses the host default (8 s); on timeout the callback fires with `error = "请求超时"`;
- prefer plain HTTP endpoints when available (that's what the translator does) so targets without TLS runtimes still work.

### Global hotkeys

- Syntax: `Alt+E`, `Ctrl+Shift+F1`; modifiers accept `Ctrl/Control`, `Alt/Option`, `Shift`, `Meta/Win/Cmd/Command/Super`;
- Keys: `A–Z`, `0–9`, `F1–F12`, `Space`, `Enter`, `Tab`, `Esc`, `Home`, `End`, `Insert`, `Del`;
- Modifiers match **exactly**: with `Alt+E` registered, `Ctrl+Alt+E` does not trigger;
- Users can reassign or clear the hotkey in Settings → Plugins (cleared = only reachable from the tray's Plugins submenu).

## Minimal plugin

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
    const std::string echo = std::string("you typed: ") + (query ? query : "");
    g_host->addResult(g_host->context, requestId, echo.c_str(),
                      "example plugin", "e", ("copy:" + echo).c_str());
    g_host->finish(g_host->context, requestId, nullptr);
    return 0;
}

static void pluginCancelQuery(unsigned long long) {}

static const ListeryPluginV1 kPlugin = {
    LISTERY_PLUGIN_API_VERSION,
    "com.example.echo", "Echo", "1.0.0", "Echoes whatever you type",
    "Alt+E",
    pluginInit, pluginShutdown, pluginBeginQuery, pluginCancelQuery, nullptr
};

extern "C" LISTERY_PLUGIN_EXPORT const ListeryPluginV1 *LISTERY_PLUGIN_CALL
listery_plugin_v1(void) { return &kPlugin; }
```

CMake (mirror of `plugins/translator/CMakeLists.txt`):

```cmake
add_library(my_plugin MODULE my_plugin.cpp)
set_target_properties(my_plugin PROPERTIES
    CXX_STANDARD 14 CXX_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
if(WIN32)
    set_target_properties(my_plugin PROPERTIES PREFIX "" SUFFIX ".dll")
endif()
target_include_directories(my_plugin PRIVATE <sdk dir>)
```

## Debugging

- Anything you send through `host->log` lands in `logs/YYYYMMDD.txt` (one file per day, 7 days kept; `LISTERY_LOG_DEBUG/INFO/WARNING`);
- Load failures (missing symbol, wrong `apiVersion`, non-zero `init`) never pop a dialog — the reason is listed under Settings → Plugins;
- `listery --selftest` loads `plugins/` and runs one real query, which is handy to validate a deployment.

## Known limitations

- Only **query plugins** exist today (`beginQuery`); no persistent panels, background tasks or custom row rendering;
- Plugins stay loaded for the process lifetime (restart Listery to pick up a new file);
- `configure` receives a native window handle — draw your own UI, don't rely on Qt;
- On Linux, Wayland sessions cannot observe global keys, so hotkeys don't work there (the tray menu still opens plugins).
