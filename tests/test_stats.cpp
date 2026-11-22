#include "netlog/stats.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using netlog::Level;
using netlog::Stats;

bool device_totals() {
    Stats stats;
    stats.add({0, "b", Level::Error, "TIMEOUT", 20});
    stats.add({1, "b", Level::Info, "OK", 10});
    stats.add({2, "a", Level::Warn, "SLOW", 7});
    const auto& devices = stats.devices();
    const auto& a = devices.at("a");
    const auto& b = devices.at("b");
    return devices.size() == 2 && a.requests == 1 && a.errors == 0
           && a.total_duration_us == 7 && a.max_duration_us == 7
           && b.requests == 2 && b.errors == 1
           && b.total_duration_us == 30 && b.max_duration_us == 20;
}

bool event_ranking() {
    Stats stats;
    if (!stats.top_events(3).empty()) {
        return false;
    }
    stats.add({0, "a", Level::Info, "Z", 1});
    stats.add({1, "a", Level::Info, "B", 1});
    stats.add({2, "b", Level::Info, "A", 1});
    stats.add({3, "b", Level::Info, "Z", 1});
    using Ranking = std::vector<Stats::EventCount>;
    return stats.top_events(0).empty()
           && stats.top_events(2) == Ranking{{"Z", 2}, {"A", 1}}
           && stats.top_events(10) == Ranking{{"Z", 2}, {"A", 1}, {"B", 1}};
}

bool overflow_preserves_totals() {
    Stats stats;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    stats.add({0, "a", Level::Info, "OK", max});
    try {
        stats.add({1, "a", Level::Error, "TIMEOUT", 1});
    } catch (const std::overflow_error&) {
        const auto& device = stats.devices().at("a");
        return device.requests == 1 && device.errors == 0
               && device.total_duration_us == max && device.max_duration_us == max
               && stats.top_events(2) == std::vector<Stats::EventCount>{{"OK", 1}};
    }
    return false;
}

bool merge_totals() {
    Stats first;
    Stats second;
    first.add({0, "shared", Level::Error, "Z", 40});
    second.add({1, "shared", Level::Warn, "Z", 80});
    // Z 在两份局部表里都排第四，合并完整计数后应排第一。
    for (const char* event : {"A", "B", "C"}) {
        first.add({2, "left", Level::Info, event, 10});
    }
    for (const char* event : {"D", "E", "F"}) {
        second.add({3, "right", Level::Info, event, 20});
    }
    first.merge(second);
    const auto& devices = first.devices();
    const auto& shared = devices.at("shared");
    return devices.size() == 3 && devices.at("left").requests == 3
           && devices.at("right").requests == 3 && shared.requests == 2 && shared.errors == 1
           && shared.total_duration_us == 120 && shared.max_duration_us == 80
           && first.top_events(3) == std::vector<Stats::EventCount>{{"Z", 2}, {"A", 1}, {"B", 1}};
}

bool merge_overflow() {
    Stats first;
    Stats second;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    first.add({0, "z", Level::Info, "OK", max});
    second.add({1, "a", Level::Info, "NEW", 10});
    second.add({2, "z", Level::Error, "TIMEOUT", 1});
    try {
        first.merge(second);
    } catch (const std::overflow_error&) {
        return first.devices().size() == 1 && first.devices().at("z").requests == 1
               && first.devices().at("z").total_duration_us == max
               && first.top_events(3) == std::vector<Stats::EventCount>{{"OK", 1}};
    }
    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string_view selected = argc == 2 ? argv[1] : "";
    const struct { std::string_view name; bool (*run)(); } cases[] = {
        {"device_totals", device_totals},
        {"event_ranking", event_ranking},
        {"overflow_preserves_totals", overflow_preserves_totals},
        {"merge_totals", merge_totals},
        {"merge_overflow", merge_overflow},
    };
    for (const auto& test : cases) {
        if (test.name == selected) {
            if (test.run()) {
                return 0;
            }
            std::cerr << "FAIL: " << selected << '\n';
            return 1;
        }
    }
    std::cerr << "Unknown statistics test: " << selected << '\n';
    return 2;
}
