#pragma once

#include "file_index.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace filesearch {

struct ScanConfig
{
    std::vector<std::wstring> roots;
    std::vector<std::wstring> excludePrefixes; // 小写、已展开环境变量
    std::vector<std::wstring> excludeNames;    // 小写目录名
    bool includeHidden = false;
};

using ScanLog = std::function<void(const std::string &)>;

/**
 * 遍历 roots 建立索引。stop 置位后尽快返回 false。
 * 扫描线程应自行设置低优先级；这里只负责文件系统遍历。
 */
bool scanFileSystem(const ScanConfig &config, Index &out, std::atomic<bool> &stop,
                    const ScanLog &log);

} // namespace filesearch
