#pragma once

#include "record.h"

#include <string_view>
#include <variant>

namespace netlog {

enum class ParseError {
    InvalidFieldCount,
    InvalidTimestamp,
    TimestampOutOfRange,
    InvalidDeviceId,
    InvalidLevel,
    InvalidEventCode,
    InvalidDuration,
    DurationOutOfRange,
};

// 二选一：成功时包含 Record，失败时包含 ParseError。
// 失败时不会向调用方暴露只填了一半的 Record。
using ParseResult = std::variant<Record, ParseError>;

// 只解析一行；不读文件、不打印、不修改外部状态。
// 输入 string_view 只在本次调用中借用，返回的 Record 自己拥有字符串。
[[nodiscard]] ParseResult parse_line(std::string_view line);
std::string_view error_message(ParseError error);

}  // namespace netlog
