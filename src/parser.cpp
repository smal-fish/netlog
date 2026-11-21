#include "netlog/parser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <system_error>

namespace netlog {
namespace {

using Fields = std::array<std::string_view, 5>;

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n'
        || c == '\v' || c == '\f';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// 只保存当前行的五个切片，不复制字段文本。
std::optional<Fields> split_fields(std::string_view line) {
    Fields fields{};
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && is_space(line[pos])) {
            ++pos;
        }
        if (pos == line.size()) {
            break;
        }
        if (count == fields.size()) {
            return std::nullopt;
        }
        const auto begin = pos;
        while (pos < line.size() && !is_space(line[pos])) {
            ++pos;
        }
        fields[count++] = line.substr(begin, pos - begin);
    }
    if (count != fields.size()) {
        return std::nullopt;
    }
    return fields;
}

bool is_identifier(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return is_digit(c) || (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
    });
}

enum class NumberError { None, Invalid, OutOfRange };

// 两个数字字段共用同一套规则，目标类型决定允许的最大值。
template <typename Integer>
NumberError parse_number(std::string_view text, Integer& value) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), is_digit)) {
        return NumberError::Invalid;
    }
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value, 10);
    if (result.ec == std::errc::result_out_of_range) {
        return NumberError::OutOfRange;
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        return NumberError::Invalid;
    }
    return NumberError::None;
}

std::optional<Level> parse_level(std::string_view text) {
    if (text == "INFO") {
        return Level::Info;
    }
    if (text == "WARN") {
        return Level::Warn;
    }
    if (text == "ERROR") {
        return Level::Error;
    }
    return std::nullopt;
}

}  // namespace

ParseResult parse_line(std::string_view line) {
    const auto split = split_fields(line);
    if (!split) {
        return ParseError::InvalidFieldCount;
    }
    const auto& fields = *split;

    std::int64_t timestamp = 0;
    const auto timestamp_error = parse_number(fields[0], timestamp);
    if (timestamp_error == NumberError::Invalid) {
        return ParseError::InvalidTimestamp;
    }
    if (timestamp_error == NumberError::OutOfRange) {
        return ParseError::TimestampOutOfRange;
    }
    if (!is_identifier(fields[1])) {
        return ParseError::InvalidDeviceId;
    }
    const auto level = parse_level(fields[2]);
    if (!level) {
        return ParseError::InvalidLevel;
    }
    if (!is_identifier(fields[3])) {
        return ParseError::InvalidEventCode;
    }
    std::uint64_t duration = 0;
    const auto duration_error = parse_number(fields[4], duration);
    if (duration_error == NumberError::Invalid) {
        return ParseError::InvalidDuration;
    }
    if (duration_error == NumberError::OutOfRange) {
        return ParseError::DurationOutOfRange;
    }
    return Record{timestamp, std::string(fields[1]), *level,
                  std::string(fields[3]), duration};
}

std::string_view error_message(ParseError error) {
    switch (error) {
        case ParseError::InvalidFieldCount:
            return "expected exactly 5 fields";
        case ParseError::InvalidTimestamp:
            return "timestamp must contain decimal digits only";
        case ParseError::TimestampOutOfRange:
            return "timestamp exceeds int64 range";
        case ParseError::InvalidDeviceId:
            return "device_id must match [A-Za-z0-9_-]+";
        case ParseError::InvalidLevel:
            return "level must be INFO, WARN or ERROR";
        case ParseError::InvalidEventCode:
            return "event_code must match [A-Za-z0-9_-]+";
        case ParseError::InvalidDuration:
            return "duration_us must contain decimal digits only";
        case ParseError::DurationOutOfRange:
            return "duration_us exceeds uint64 range";
    }
    return "unknown parse error";
}

}  // namespace netlog
