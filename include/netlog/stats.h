#pragma once

#include "netlog/record.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace netlog {

struct DeviceStats {
    std::uint64_t requests = 0;
    std::uint64_t errors = 0;
    std::uint64_t total_duration_us = 0;
    std::uint64_t max_duration_us = 0;
};

class Stats {
public:
    using DeviceTable = std::map<std::string, DeviceStats>;
    using EventCount = std::pair<std::string, std::uint64_t>;

    void add(const Record& record);
    void merge(const Stats& other);
    const DeviceTable& devices() const;
    std::vector<EventCount> top_events(std::size_t limit) const;

private:
    DeviceTable devices_;
    std::unordered_map<std::string, std::uint64_t> events_;
};

}  // namespace netlog
