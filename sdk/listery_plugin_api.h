/*
 * Listery 插件接口 v1（纯 C ABI / Listery Plugin API v1 — pure C ABI）
 *
 * 设计目标 / Design goals
 *  - 跨编译器、跨 Qt 版本可用：接口只有 C 结构体与函数指针，不传递任何 C++ 对象；
 *  - 插件零依赖：不需要 Qt、不需要 OpenSSL，网络请求由宿主代劳；
 *  - 单向依赖：插件只调用宿主提供的函数，宿主从不回调插件的 C++ 类型。
 *
 * 插件导出一个符号（C 链接）：
 *     const ListeryPluginV1 *listery_plugin_v1(void);
 *
 * 最小示例（C++）见 sdk/example/ 与 plugins/translator/。
 *
 * ---------------------------------------------------------------------------
 * 版本规则 / Versioning
 *   ListeryPluginV1.apiVersion 必须填 LISTERY_PLUGIN_API_VERSION。
 *   宿主只在其 apiVersion >= 宿主支持版本时加载。新增字段一律追加到结构体末尾，
 *   插件在调用扩展函数前必须检查 ListeryHostV1.apiVersion。
 */

#ifndef LISTERY_PLUGIN_API_H
#define LISTERY_PLUGIN_API_H

#include <stddef.h>

/*
 * 1 → 2：列定义增加可选的排序键后缀，插件描述符末尾增加 setResultSort。
 * 宿主仍接受 apiVersion 1 的老插件（老插件没有排序键，表头保持只读）。
 */
#define LISTERY_PLUGIN_API_VERSION 2

/* -------------------------------------------------------------------------- */
/* 导出宏 / Export macros                                                      */
/* -------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(_WIN64)
#  define LISTERY_PLUGIN_EXPORT __declspec(dllexport)
#  define LISTERY_PLUGIN_CALL __cdecl
#else
#  define LISTERY_PLUGIN_EXPORT __attribute__((visibility("default")))
#  define LISTERY_PLUGIN_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** 日志级别 / Log levels。 */
enum ListeryLogLevel {
    LISTERY_LOG_DEBUG = 0,
    LISTERY_LOG_INFO = 1,
    LISTERY_LOG_WARNING = 2
};

/**
 * HTTP GET 回调 / HTTP GET callback。
 *
 * 在宿主的网络线程（通常是主线程）里被调用，插件不应在回调里阻塞。
 * statusCode：HTTP 状态码，网络失败时为 0；errorMessage 为 NULL 表示成功。
 * body 只在回调期间有效。
 */
typedef void(LISTERY_PLUGIN_CALL *ListeryHttpCallback)(void *userData,
                                                       int statusCode,
                                                       const char *body,
                                                       unsigned int bodySize,
                                                       const char *errorMessage);

/**
 * 宿主能力表 / Host capability table。
 *
 * 插件在 init() 里拿到指针后应自行保存一份（宿主保证指针在整个生命周期有效）。
 */
typedef struct ListeryHostV1 {
    /** 宿主接口版本：插件调用扩展函数前必须检查。 */
    int apiVersion;
    /** 不透明上下文，调用下面每个函数时原样传回。 */
    void *context;

    /**
     * 提交一行结果 / Submit one result row。
     *   title    主标题（必填）
     *   subtitle 副标题（可 NULL）
     *   badge    行首徽标文字，建议 1~2 个字符，如 "译"（可 NULL）
     *   action   选中回车时执行的动作，见下方 action 协议（可 NULL 表示无动作）
     * 所有字符串都会被宿主拷贝，回调返回后即可释放。可从任意线程调用。
     */
    void(LISTERY_PLUGIN_CALL *addResult)(void *context,
                                         unsigned long long requestId,
                                         const char *title,
                                         const char *subtitle,
                                         const char *badge,
                                         const char *action);

    /**
     * 结束一次查询 / Finish a query。
     * errorMessage 为 NULL 表示正常结束；否则会把错误显示在面板状态栏。
     * 必须调用，否则宿主不知道请求已结束（也就不刷新界面）。
     */
    void(LISTERY_PLUGIN_CALL *finish)(void *context,
                                      unsigned long long requestId,
                                      const char *errorMessage);

    /** 写日志 / Write a log line（level 见 ListeryLogLevel）。 */
    void(LISTERY_PLUGIN_CALL *log)(void *context, int level, const char *message);

    /**
     * 异步 HTTP GET / Asynchronous HTTP GET。
     *   headers    形如 "Accept: application/json\nUser-Agent: listery"（可 NULL）
     *   timeoutMs  超时毫秒数（<=0 表示用宿主默认，通常 8 秒）
     * 返回 0 表示已发起；非 0 表示无法发起（此时不会回调）。
     */
    int(LISTERY_PLUGIN_CALL *httpGet)(void *context,
                                      const char *url,
                                      const char *headers,
                                      ListeryHttpCallback callback,
                                      void *userData,
                                      int timeoutMs);

    /*
     * ---- 可选扩展：列视图（Everything 风格），追加在结构体末尾以保持 ABI 兼容 ----
     *
     * 老宿主没有这两个字段，所以插件必须先判断函数指针非 NULL 再用；为 NULL 时
     * 退回 addResult。宿主保证能力表指针在整个生命周期有效。
     *
     * setResultColumns：在提交任何结果行之前调用一次。
     *   spec 是 '\x1f'（ASCII US）分隔的列定义，每列格式 "标题:宽度:对齐[:排序键[:方向]]"：
     *     宽度 0 = 弹性列（与其他弹性列平分剩余宽度），>0 = 固定像素宽度；
     *     对齐 l = 左对齐，r = 右对齐。标题中不要出现 ':'。
     *     排序键（可选）由插件定义，点击表头时宿主原样交给 setResultSort；
     *       没有排序键的列不可点击（老插件/纯展示列）。
     *     方向（可选）asc / desc 只在“当前正按该列排序”时填写，用来画排序箭头。
     *   例："名称:0:l:name:asc\x1f大小:92:r:size\x1f修改日期:140:l:mtime"
     *   传 NULL / 空串表示本次查询不使用列视图。
     *
     * addResultRow：与 addResult 语义完全相同，额外附带与列定义等长、同样以
     *   '\x1f' 分隔的单元格文本。title/subtitle/badge/action 仍需填写
     *   （subtitle 会作为列视图下的 tooltip 兜底）。
     */
    void(LISTERY_PLUGIN_CALL *setResultColumns)(void *context,
                                                unsigned long long requestId,
                                                const char *spec);
    void(LISTERY_PLUGIN_CALL *addResultRow)(void *context,
                                            unsigned long long requestId,
                                            const char *title,
                                            const char *subtitle,
                                            const char *badge,
                                            const char *action,
                                            const char *cells);
} ListeryHostV1;

/**
 * 插件描述 / Plugin descriptor。
 *
 * 由插件返回一个静态（进程生命周期内常驻）的结构体。
 */
typedef struct ListeryPluginV1 {
    int apiVersion;

    /** 稳定标识，用于保存启用状态与快捷键，如 "com.listery.translator"。 */
    const char *id;
    /** 显示名，如 "英汉翻译"。 */
    const char *name;
    /** 版本字符串，如 "1.0.0"。 */
    const char *version;
    /** 一句话说明，显示在插件管理页。 */
    const char *description;
    /** 默认全局热键，如 "Alt+E"；为空表示不占用热键（只能通过插件菜单唤起）。 */
    const char *defaultHotkey;

    /** 初始化：返回 0 成功，非 0 时宿主会卸载该插件并把返回值当错误码记录。 */
    int(LISTERY_PLUGIN_CALL *init)(const ListeryHostV1 *host);
    /** 退出：宿主保证在进程结束前调用一次，结束后不再收到任何回调。 */
    void(LISTERY_PLUGIN_CALL *shutdown)(void);

    /**
     * 开始一次查询 / Begin a query。
     * 返回 0 表示接受该请求（之后必须调用 finish）；非 0 表示不接受（宿主立即结束）。
     * requestId 全局唯一，用于把 addResult / finish 对应到本次输入。
     * 查询可以异步：宿主不会等待，新的输入到来时旧 result 会被丢弃。
     */
    int(LISTERY_PLUGIN_CALL *beginQuery)(unsigned long long requestId,
                                         const char *query,
                                         int maxResults);
    /** 取消（面板已隐藏或输入已变化）：插件可以忽略，但应尽快停止网络动作。 */
    void(LISTERY_PLUGIN_CALL *cancelQuery)(unsigned long long requestId);

    /**
     * 可选：打开插件自己的设置界面（nativeParentWindow 是宿主窗口的原生句柄）。
     * 不需要就填 NULL。插件应避免依赖 Qt：可以用原生 API 或写自己的配置文件。
     */
    int(LISTERY_PLUGIN_CALL *configure)(void *nativeParentWindow);

    /*
     * 可选扩展：表头点击排序（追加在结构体末尾，apiVersion >= 2）。
     *
     * columnKey 是 setResultColumns spec 里该列的排序键；ascending 非 0 表示升序，
     * 0 表示降序。插件应把排序偏好持久化，并在下一次 beginQuery 时按新顺序返回
     * “整个结果集”的前 N 条（不能只重排当前面板里的十几行）。
     * 返回 0 表示接受并已生效，非 0 时宿主不改表头箭头。
     * 老插件该字段为 NULL，宿主不会调用。
     */
    int(LISTERY_PLUGIN_CALL *setResultSort)(const char *columnKey, int ascending);
} ListeryPluginV1;

/**
 * 插件入口 / Plugin entry point（必须导出，C 链接）。
 * 返回的结构体必须是静态存储期的，宿主不会拷贝。
 */
typedef const ListeryPluginV1 *(LISTERY_PLUGIN_CALL *ListeryPluginEntry)(void);

/* -------------------------------------------------------------------------- */
/* action 协议 / action protocol（宿主负责执行）                                */
/* -------------------------------------------------------------------------- */
/*
 *   "copy:<text>"   把 <text> 复制到剪贴板
 *   "url:<url>"     用系统默认浏览器打开 <url>
 *   "run:<path>"    启动 <path>（可执行文件）
 *   "open:<path>"   在文件管理器里显示 <path>
 *   空 / NULL       Enter 不执行动作（只隐藏面板）
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LISTERY_PLUGIN_API_H */
