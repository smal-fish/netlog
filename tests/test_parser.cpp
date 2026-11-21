#include "netlog/parser.h"

#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
int checks = 0;

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}
}  // namespace

int main() {
    using netlog::Level;
    using netlog::ParseError;
    using netlog::Record;

    const auto sample = netlog::parse_line("1700000000 dev01 INFO REQ_OK 100");
    const auto* record = std::get_if<Record>(&sample);
    check(record != nullptr, "sample accepted");
    if (record != nullptr) {
        check(record->timestamp == 1700000000, "timestamp value");
        check(record->device_id == "dev01", "device value");
        check(record->level == Level::Info, "INFO value");
        check(record->event_code == "REQ_OK", "event value");
        check(record->duration_us == 100, "duration value");
    }

    const auto bounds = netlog::parse_line(
        "9223372036854775807 dev-01_X ERROR NEW_EVENT 18446744073709551615");
    const auto* maximum = std::get_if<Record>(&bounds);
    check(maximum != nullptr, "numeric maxima accepted");
    if (maximum != nullptr) {
        check(maximum->timestamp == std::numeric_limits<std::int64_t>::max(), "int64 max value");
        check(maximum->duration_us == std::numeric_limits<std::uint64_t>::max(), "uint64 max value");
        check(maximum->level == Level::Error, "ERROR value");
    }

    const auto spaced = netlog::parse_line(" \t000 dev_1\tWARN\tNEW-CODE 0000\r ");
    const auto* warning = std::get_if<Record>(&spaced);
    check(warning != nullptr, "whitespace and leading zeros accepted");
    if (warning != nullptr) {
        check(warning->timestamp == 0 && warning->duration_us == 0, "zero values");
        check(warning->level == Level::Warn, "WARN value");
    }

    struct BadCase { std::string_view line; ParseError error; };
    const std::vector<BadCase> cases = {
        {"", ParseError::InvalidFieldCount},
        {" \t\r", ParseError::InvalidFieldCount},
        {"broken_line", ParseError::InvalidFieldCount},
        {"0 dev INFO OK", ParseError::InvalidFieldCount},
        {"0 dev INFO OK 1 extra", ParseError::InvalidFieldCount},
        {"0 dev INFO OK 1 # comment", ParseError::InvalidFieldCount},
        {"-1 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"+1 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"1.0 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"1abc dev INFO OK 1", ParseError::InvalidTimestamp},
        {"0x10 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"1e2 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"9223372036854775808 dev INFO OK 1", ParseError::TimestampOutOfRange},
        {"999999999999999999999999999999999999 dev INFO OK 1", ParseError::TimestampOutOfRange},
        {"0 dev.01 INFO OK 1", ParseError::InvalidDeviceId},
        {"0 dev/01 INFO OK 1", ParseError::InvalidDeviceId},
        {"0 dev info OK 1", ParseError::InvalidLevel},
        {"0 dev DEBUG OK 1", ParseError::InvalidLevel},
        {"0 dev INFO BAD.CODE 1", ParseError::InvalidEventCode},
        {"0 dev INFO OK -1", ParseError::InvalidDuration},
        {"0 dev INFO OK +1", ParseError::InvalidDuration},
        {"0 dev INFO OK 1.5", ParseError::InvalidDuration},
        {"0 dev INFO OK 1abc", ParseError::InvalidDuration},
        {"0 dev INFO OK 0x10", ParseError::InvalidDuration},
        {"0 dev INFO OK 18446744073709551616", ParseError::DurationOutOfRange},
        {"0 dev INFO OK 999999999999999999999999999999999999", ParseError::DurationOutOfRange},
        {"bad dev.1 DEBUG BAD.CODE -1", ParseError::InvalidTimestamp},
    };
    for (const auto& test : cases) {
        const auto result = netlog::parse_line(test.line);
        const auto* error = std::get_if<ParseError>(&result);
        check(error != nullptr && *error == test.error, test.line);
    }

    const std::string bom = std::string("\xEF\xBB\xBF") + "0 dev INFO OK 1";
    const auto bom_result = netlog::parse_line(bom);
    const auto* bom_error = std::get_if<ParseError>(&bom_result);
    check(bom_error != nullptr && *bom_error == ParseError::InvalidTimestamp, "BOM rejected");

    std::string with_nul = "0 dev INFO OK 1";
    with_nul.push_back('\0');
    const auto nul_result = netlog::parse_line(with_nul);
    const auto* nul_error = std::get_if<ParseError>(&nul_result);
    check(nul_error != nullptr && *nul_error == ParseError::InvalidDuration, "embedded NUL rejected");

    std::string source = "0 original INFO ORIGINAL_EVENT 1";
    const auto owned = netlog::parse_line(source);
    source.assign(1000, 'x');
    const auto* saved = std::get_if<Record>(&owned);
    check(saved != nullptr && saved->device_id == "original"
          && saved->event_code == "ORIGINAL_EVENT", "record owns string data");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
