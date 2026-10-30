/*
 * Listery 词典翻译插件核心 / Dictionary translator core（header-only）
 *
 * 英汉、法汉等插件共用同一份实现：调用方在 include 之前用宏描述自己的
 * id、名称、热键、徽标与语种，这里只做「输入外语给汉语，输入汉语给外语」。
 * 依赖：只有 C/C++ 标准库 —— 不链接 Qt、不需要 OpenSSL，网络请求由宿主的
 * httpGet 代劳，因此同一份源码可以编译到 Qt 5.7(XP) / Qt 5.15 / Qt 6 的 Listery。
 *
 * 数据源：有道词典 suggest 接口（明文 HTTP，双向查词）。
 * 可用环境变量覆盖（变量名由 LISTERY_DICT_ENV 指定）：
 *   <env>=youdao|google   （默认 youdao；google 走 HTTPS，
 *                          在缺少 TLS 运行库的环境可能失败）
 *
 * 调用方必须定义：
 *   LISTERY_DICT_ID              插件标识，如 "com.listery.french"
 *   LISTERY_DICT_NAME            显示名，如 "法汉翻译"
 *   LISTERY_DICT_DESCRIPTION     一句话说明
 *   LISTERY_DICT_HOTKEY          默认热键，如 "Alt+F"
 *   LISTERY_DICT_BADGE           行首徽标，如 "法"
 *   LISTERY_DICT_LE_FOREIGN      有道的 le 参数（外语侧）
 *   LISTERY_DICT_GOOGLE_FOREIGN  Google 的外语语言码（"en" / "fr" / ...）
 *   LISTERY_DICT_LOG_NAME        日志前缀 / User-Agent 中的名字
 *   LISTERY_DICT_ENV             覆盖数据源的环境变量名
 */

#ifndef LISTERY_DICT_ID
#  error "translator_core.h 需要先定义 LISTERY_DICT_* 宏"
#endif

#include "listery_plugin_api.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

const ListeryHostV1 *g_host = nullptr;
std::mutex g_mutex;
/** 还在等待网络回包的请求；取消后回调里的结果会被丢弃。 */
std::set<unsigned long long> g_active;

struct Request
{
    unsigned long long id = 0;
    int maxResults = 8;
};

const int kMaxEntries = 8;
const int kTimeoutMs = 6000;

std::string trim(const std::string &text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ')
        ++begin;
    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ')
        --end;
    return text.substr(begin, end - begin);
}

/** 判断是否是中日韩统一表意文字（简化判断：UTF-8 三字节且落在常用区）。 */
bool containsCjk(const std::string &text)
{
    for (size_t i = 0; i + 2 < text.size();) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        if (c0 >= 0xE0 && c0 <= 0xEF) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            const unsigned int code = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            if (code >= 0x4E00 && code <= 0x9FFF)
                return true;
            i += 3;
            continue;
        }
        if (c0 >= 0xF0)
            i += 4;
        else if (c0 >= 0xE0)
            i += 3;
        else if (c0 >= 0xC0)
            i += 2;
        else
            i += 1;
    }
    return false;
}

std::string urlEncode(const std::string &value)
{
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 2);
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string &out, unsigned int code)
{
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

/** 处理 JSON 字符串里的转义：支持 \\ \" \/ \n \t \r \b \f 与 \uXXXX（含代理对）。 */
std::string unescape(const std::string &raw)
{
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size()) {
            out.push_back(c);
            continue;
        }
        const char next = raw[++i];
        switch (next) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
            unsigned int code = 0;
            bool ok = i + 4 < raw.size();
            for (int k = 1; k <= 4 && ok; ++k) {
                const int digit = hexValue(raw[i + k]);
                if (digit < 0)
                    ok = false;
                else
                    code = (code << 4) | static_cast<unsigned int>(digit);
            }
            if (!ok) {
                out.push_back('u');
                break;
            }
            i += 4;
            // 高位代理 + 低位代理 → 真正的码点
            if (code >= 0xD800 && code <= 0xDBFF && i + 6 < raw.size() && raw[i + 1] == '\\'
                && raw[i + 2] == 'u') {
                unsigned int low = 0;
                bool lowOk = true;
                for (int k = 3; k <= 6; ++k) {
                    const int digit = hexValue(raw[i + k]);
                    if (digit < 0)
                        lowOk = false;
                    else
                        low = (low << 4) | static_cast<unsigned int>(digit);
                }
                if (lowOk && low >= 0xDC00 && low <= 0xDFFF) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    i += 6;
                }
            }
            appendUtf8(out, code);
            break;
        }
        default:
            out.push_back(next);
            break;
        }
    }
    return out;
}

/** 取出 "key":"value" 的 value（从 from 开始找），找不到返回 false。 */
bool extractString(const std::string &json, const char *key, size_t &from, std::string &value)
{
    const std::string pattern = std::string("\"") + key + "\":\"";
    const size_t pos = json.find(pattern, from);
    if (pos == std::string::npos)
        return false;

    size_t i = pos + pattern.size();
    std::string raw;
    bool closed = false;
    for (; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '\\') {
            raw.push_back(c);
            if (i + 1 < json.size())
                raw.push_back(json[++i]);
            continue;
        }
        if (c == '"') {
            closed = true;
            break;
        }
        raw.push_back(c);
    }
    from = i + 1;
    if (!closed)
        return false;
    value = unescape(raw);
    return true;
}

struct Entry
{
    std::string word;
    std::string explain;
};

/** 去掉 <b>...</b> 之类的标签（有道部分接口会返回带标签的片段）。 */
std::string stripTags(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    bool inTag = false;
    for (char c : text) {
        if (c == '<')
            inTag = true;
        else if (c == '>')
            inTag = false;
        else if (!inTag)
            out.push_back(c);
    }
    return trim(out);
}

std::string firstLine(const std::string &text)
{
    const size_t pos = text.find_first_of("\r\n");
    std::string line = pos == std::string::npos ? text : text.substr(0, pos);
    // 面板一行放不下太长的释义：按字节截到 120 以内，但必须落在 UTF-8 字符边界上。
    // 直接按字节切会把一个汉字劈成两半，Qt 解码后标题末尾会冒出“�”
    const size_t maxBytes = 120;
    if (line.size() > maxBytes) {
        size_t cut = maxBytes;
        // 10xxxxxx 是续字节，往回退到该字符的起始字节
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
            --cut;
        line = line.substr(0, cut) + "...";
    }
    return line;
}

/** 解析有道 suggest：entries 数组里按顺序出现的 explain / entry 配对。 */
std::vector<Entry> parseYoudao(const std::string &body)
{
    std::vector<Entry> entries;
    size_t pos = body.find("\"entries\"");
    if (pos == std::string::npos)
        return entries;

    std::string pendingExplain;
    bool hasExplain = false;
    while (entries.size() < static_cast<size_t>(kMaxEntries)) {
        const size_t explainAt = body.find("\"explain\":\"", pos);
        const size_t entryAt = body.find("\"entry\":\"", pos);
        if (explainAt == std::string::npos && entryAt == std::string::npos)
            break;

        if (explainAt != std::string::npos
            && (entryAt == std::string::npos || explainAt < entryAt)) {
            std::string value;
            if (!extractString(body, "explain", pos, value))
                break;
            pendingExplain = stripTags(value);
            hasExplain = true;
            continue;
        }

        std::string word;
        if (!extractString(body, "entry", pos, word))
            break;
        Entry entry;
        entry.word = stripTags(word);
        entry.explain = hasExplain ? pendingExplain : std::string();
        pendingExplain.clear();
        hasExplain = false;
        if (entry.word.empty() && entry.explain.empty())
            continue;
        entries.push_back(entry);
    }

    return entries;
}

/** 解析 Google gtx：[["译文","原文",...],...]，只取每段第一个字符串。 */
std::vector<Entry> parseGoogle(const std::string &body)
{
    std::vector<Entry> entries;
    size_t pos = 0;
    while (entries.size() < static_cast<size_t>(kMaxEntries)) {
        const size_t bracket = body.find("[\"", pos);
        if (bracket == std::string::npos)
            break;

        std::string raw;
        size_t i = bracket + 2;
        bool closed = false;
        for (; i < body.size(); ++i) {
            const char c = body[i];
            if (c == '\\') {
                raw.push_back(c);
                if (i + 1 < body.size())
                    raw.push_back(body[++i]);
                continue;
            }
            if (c == '"') {
                closed = true;
                break;
            }
            raw.push_back(c);
        }
        if (!closed)
            break;
        pos = i + 1;

        const std::string text = stripTags(unescape(raw));
        if (text.empty())
            continue;
        Entry entry;
        entry.explain = text;
        entry.word = text;
        entries.push_back(entry);
    }
    return entries;
}

std::string currentEndpoint()
{
    const char *value = std::getenv(LISTERY_DICT_ENV);
    if (value == nullptr || value[0] == '\0')
        return std::string("youdao");
    std::string endpoint(value);
    for (size_t i = 0; i < endpoint.size(); ++i)
        endpoint[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(endpoint[i])));
    return endpoint;
}

std::string buildUrl(const std::string &query, bool chinese)
{
    const std::string encoded = urlEncode(query);
    if (currentEndpoint() == "google") {
        return std::string("https://translate.googleapis.com/translate_a/single?client=gtx&sl=")
               + (chinese ? "zh-CN" : LISTERY_DICT_GOOGLE_FOREIGN) + "&tl="
               + (chinese ? LISTERY_DICT_GOOGLE_FOREIGN : "zh-CN") + "&dt=t&q=" + encoded;
    }
    return std::string("http://dict.youdao.com/suggest?num=") + std::to_string(kMaxEntries)
           + "&ver=3.0&doctype=json&cache=false&le="
           + (chinese ? "zh" : LISTERY_DICT_LE_FOREIGN) + "&q=" + encoded;
}

void finishWithError(unsigned long long requestId, const std::string &message)
{
    if (g_host == nullptr || g_host->finish == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active.erase(requestId) == 0)
            return; // 已被取消或已结束
    }
    g_host->finish(g_host->context, requestId, message.c_str());
}

void LISTERY_PLUGIN_CALL onHttpResponse(void *userData,
                                        int statusCode,
                                        const char *body,
                                        unsigned int bodySize,
                                        const char *errorMessage)
{
    Request *request = static_cast<Request *>(userData);
    if (request == nullptr)
        return;
    const unsigned long long id = request->id;
    const int limit = request->maxResults > 0 ? request->maxResults : kMaxEntries;
    delete request;

    // 已取消：宿主早已收到 finish，这里直接丢弃
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active.erase(id) == 0)
            return;
    }

    if (g_host == nullptr)
        return;

    if (errorMessage != nullptr) {
        g_host->finish(g_host->context, id, errorMessage);
        return;
    }
    if (statusCode != 200 || body == nullptr || bodySize == 0) {
        const std::string message = std::string("翻译服务返回异常（HTTP ")
                                    + std::to_string(statusCode) + "）";
        g_host->finish(g_host->context, id, message.c_str());
        return;
    }

    const std::string payload(body, bodySize);
    std::vector<Entry> entries = currentEndpoint() == "google" ? parseGoogle(payload)
                                                               : parseYoudao(payload);
    if (entries.empty()) {
        g_host->finish(g_host->context, id, "没有找到释义");
        return;
    }

    int submitted = 0;
    for (const Entry &entry : entries) {
        if (submitted >= limit)
            break;
        const std::string title = firstLine(entry.explain);
        const std::string subtitle = firstLine(entry.word);
        if (title.empty())
            continue;
        const std::string action = "copy:" + title;
        g_host->addResult(g_host->context, id, title.c_str(),
                          subtitle.empty() ? nullptr : subtitle.c_str(),
                          LISTERY_DICT_BADGE, action.c_str());
        ++submitted;
    }
    g_host->finish(g_host->context, id, nullptr);
}

} // namespace

// ============================== 插件接口实现 ==============================
static int LISTERY_PLUGIN_CALL pluginInit(const ListeryHostV1 *host)
{
    if (host == nullptr || host->apiVersion < LISTERY_PLUGIN_API_VERSION)
        return 1;
    if (host->addResult == nullptr || host->finish == nullptr || host->httpGet == nullptr)
        return 2;
    g_host = host;
    if (g_host->log) {
        const std::string message =
            std::string(LISTERY_DICT_LOG_NAME ": 已加载，数据源 ") + currentEndpoint();
        g_host->log(g_host->context, LISTERY_LOG_INFO, message.c_str());
    }
    return 0;
}

static void LISTERY_PLUGIN_CALL pluginShutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active.clear();
    g_host = nullptr;
}

static int LISTERY_PLUGIN_CALL pluginBeginQuery(unsigned long long requestId,
                                                const char *query,
                                                int maxResults)
{
    if (g_host == nullptr)
        return 1;

    const std::string text = trim(query == nullptr ? std::string() : std::string(query));
    if (text.empty()) {
        g_host->finish(g_host->context, requestId, nullptr);
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_active.insert(requestId);
    }

    Request *request = new Request();
    request->id = requestId;
    request->maxResults = maxResults > 0 ? maxResults : kMaxEntries;

    const std::string url = buildUrl(text, containsCjk(text));
    const int rc = g_host->httpGet(g_host->context, url.c_str(),
                                   "User-Agent: listery-" LISTERY_DICT_LOG_NAME
                                   "/1.0\r\nAccept: application/json",
                                   onHttpResponse, request, kTimeoutMs);
    if (rc != 0) {
        delete request;
        finishWithError(requestId, "无法发起网络请求");
        return 0;
    }
    return 0;
}

static void LISTERY_PLUGIN_CALL pluginCancelQuery(unsigned long long requestId)
{
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        removed = g_active.erase(requestId) != 0;
    }
    if (removed && g_host != nullptr && g_host->finish != nullptr)
        g_host->finish(g_host->context, requestId, nullptr);
}

static const ListeryPluginV1 kPlugin = {
    LISTERY_PLUGIN_API_VERSION,
    LISTERY_DICT_ID,
    LISTERY_DICT_NAME,
    "1.0.0",
    LISTERY_DICT_DESCRIPTION,
    LISTERY_DICT_HOTKEY,
    pluginInit,
    pluginShutdown,
    pluginBeginQuery,
    pluginCancelQuery,
    nullptr // 暂无独立设置界面
};

extern "C" LISTERY_PLUGIN_EXPORT const ListeryPluginV1 *LISTERY_PLUGIN_CALL listery_plugin_v1(void)
{
    return &kPlugin;
}
