#pragma once

#include <cstdint>
#include <string>

namespace netlog {

enum class Level { Info, Warn, Error };

// 一条记录代表一次请求处理的最终结果。
struct Record {
    std::int64_t timestamp = 0;
    std::string device_id;
    Level level = Level::Info;
    std::string event_code;
    std::uint64_t duration_us = 0;
};

}  // namespace netlog
