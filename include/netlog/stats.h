#pragma once

#include "record.h"

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
    using BucketTable = std::map<std::int64_t, DeviceStats>;
    using EventCount = std::pair<std::string, std::uint64_t>;

    explicit Stats(std::int64_t bucket_seconds = 0);
    void add(const Record& record);
    void merge(const Stats& other);
    const DeviceTable& devices() const;
    const BucketTable& buckets() const;
    std::int64_t bucket_seconds() const;
    std::vector<EventCount> top_events(std::size_t limit) const;

private:
    std::int64_t bucket_seconds_;
    DeviceTable devices_;
    BucketTable buckets_;
    std::unordered_map<std::string, std::uint64_t> events_;
};

}  // namespace netlog
