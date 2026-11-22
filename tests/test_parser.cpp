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

int main(int argc, char* argv[]) {
    const std::string_view selected = argc == 2 ? argv[1] : "";
    bool found = false;
    const auto run_case = [&](std::string_view name, auto run) {
        if (selected.empty() || selected == name) {
            found = true;
            run();
        }
    };
    using netlog::Level;
    using netlog::ParseError;
    using netlog::Record;

    run_case("valid_fields", [] {
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
    });

    run_case("numeric_maxima", [] {
        const auto bounds = netlog::parse_line(
            "9223372036854775807 dev-01_X ERROR NEW_EVENT 18446744073709551615");
        const auto* maximum = std::get_if<Record>(&bounds);
        check(maximum != nullptr, "numeric maxima accepted");
        if (maximum != nullptr) {
            check(maximum->timestamp == std::numeric_limits<std::int64_t>::max(), "int64 max value");
            check(maximum->duration_us == std::numeric_limits<std::uint64_t>::max(), "uint64 max value");
            check(maximum->level == Level::Error, "ERROR value");
        }
    });

    run_case("whitespace_and_zeros", [] {
        const auto spaced = netlog::parse_line(" \t000 dev_1\tWARN\tNEW-CODE 0000\r ");
        const auto* warning = std::get_if<Record>(&spaced);
        check(warning != nullptr, "whitespace and leading zeros accepted");
        if (warning != nullptr) {
            check(warning->timestamp == 0 && warning->duration_us == 0, "zero values");
            check(warning->level == Level::Warn, "WARN value");
        }
    });

    struct BadCase {
        std::string_view name;
        std::string_view line;
        ParseError error;
    };
    const std::vector<BadCase> cases = {
        {"empty_line", "", ParseError::InvalidFieldCount},
        {"blank_line", " \t\r", ParseError::InvalidFieldCount},
        {"one_field", "broken_line", ParseError::InvalidFieldCount},
        {"missing_field", "0 dev INFO OK", ParseError::InvalidFieldCount},
        {"extra_field", "0 dev INFO OK 1 extra", ParseError::InvalidFieldCount},
        {"comment", "0 dev INFO OK 1 # comment", ParseError::InvalidFieldCount},
        {"negative_timestamp", "-1 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"signed_timestamp", "+1 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"fractional_timestamp", "1.0 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"timestamp_suffix", "1abc dev INFO OK 1", ParseError::InvalidTimestamp},
        {"hex_timestamp", "0x10 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"scientific_timestamp", "1e2 dev INFO OK 1", ParseError::InvalidTimestamp},
        {"timestamp_overflow", "9223372036854775808 dev INFO OK 1", ParseError::TimestampOutOfRange},
        {"huge_timestamp", "999999999999999999999999999999999999 dev INFO OK 1", ParseError::TimestampOutOfRange},
        {"dotted_id", "0 dev.01 INFO OK 1", ParseError::InvalidDeviceId},
        {"slash_id", "0 dev/01 INFO OK 1", ParseError::InvalidDeviceId},
        {"lowercase_level", "0 dev info OK 1", ParseError::InvalidLevel},
        {"unknown_level", "0 dev DEBUG OK 1", ParseError::InvalidLevel},
        {"dotted_event", "0 dev INFO BAD.CODE 1", ParseError::InvalidEventCode},
        {"negative_duration", "0 dev INFO OK -1", ParseError::InvalidDuration},
        {"signed_duration", "0 dev INFO OK +1", ParseError::InvalidDuration},
        {"fractional_duration", "0 dev INFO OK 1.5", ParseError::InvalidDuration},
        {"duration_suffix", "0 dev INFO OK 1abc", ParseError::InvalidDuration},
        {"hex_duration", "0 dev INFO OK 0x10", ParseError::InvalidDuration},
        {"duration_overflow", "0 dev INFO OK 18446744073709551616", ParseError::DurationOutOfRange},
        {"huge_duration", "0 dev INFO OK 999999999999999999999999999999999999", ParseError::DurationOutOfRange},
        {"first_error", "bad dev.1 DEBUG BAD.CODE -1", ParseError::InvalidTimestamp},
    };
    for (const auto& test : cases) {
        run_case(test.name, [&] {
            const auto result = netlog::parse_line(test.line);
            const auto* error = std::get_if<ParseError>(&result);
            check(error != nullptr && *error == test.error, test.name);
        });
    }

    run_case("bom", [] {
        const std::string bom = std::string("\xEF\xBB\xBF") + "0 dev INFO OK 1";
        const auto bom_result = netlog::parse_line(bom);
        const auto* bom_error = std::get_if<ParseError>(&bom_result);
        check(bom_error != nullptr && *bom_error == ParseError::InvalidTimestamp, "BOM rejected");
    });

    run_case("embedded_nul", [] {
        std::string with_nul = "0 dev INFO OK 1";
        with_nul.push_back('\0');
        const auto nul_result = netlog::parse_line(with_nul);
        const auto* nul_error = std::get_if<ParseError>(&nul_result);
        check(nul_error != nullptr && *nul_error == ParseError::InvalidDuration, "embedded NUL rejected");
    });

    run_case("owned_strings", [] {
        std::string source = "0 original INFO ORIGINAL_EVENT 1";
        const auto owned = netlog::parse_line(source);
        source.assign(1000, 'x');
        const auto* saved = std::get_if<Record>(&owned);
        check(saved != nullptr && saved->device_id == "original"
              && saved->event_code == "ORIGINAL_EVENT", "record owns string data");
    });

    if (!found) {
        std::cerr << "Unknown parser test: " << selected << '\n';
        return 2;
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
