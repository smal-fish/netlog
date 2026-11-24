#include "netlog/stats.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace netlog {
namespace {

void check_duration_sum(const std::string& id, std::uint64_t total, std::uint64_t extra) {
    if (extra > std::numeric_limits<std::uint64_t>::max() - total) {
        throw std::overflow_error("total duration for device " + id + " exceeds uint64 range");
    }
}

void check_bucket_duration(std::int64_t start, std::uint64_t total, std::uint64_t extra) {
    if (extra > std::numeric_limits<std::uint64_t>::max() - total) {
        throw std::overflow_error("total duration for time window starting at "
                                  + std::to_string(start) + " exceeds uint64 range");
    }
}

void add_totals(DeviceStats& totals, const DeviceStats& incoming) {
    totals.requests += incoming.requests;
    totals.errors += incoming.errors;
    totals.total_duration_us += incoming.total_duration_us;
    totals.max_duration_us = std::max(totals.max_duration_us, incoming.max_duration_us);
}

}  // namespace

Stats::Stats(std::int64_t bucket_seconds) : bucket_seconds_(bucket_seconds) {
    if (bucket_seconds < 0) {
        throw std::invalid_argument("time window width must be non-negative");
    }
}

void Stats::add(const Record& record) {
    std::int64_t bucket_start = 0;
    DeviceStats* bucket_totals = nullptr;
    if (bucket_seconds_ > 0) {
        // timestamp 来自已校验的非负 Unix 秒，按窗口宽度向下对齐。
        bucket_start = record.timestamp - record.timestamp % bucket_seconds_;
        const auto bucket = buckets_.find(bucket_start);
        if (bucket != buckets_.end()) {
            check_bucket_duration(bucket_start, bucket->second.total_duration_us, record.duration_us);
            bucket_totals = &bucket->second;
        }
    }

    auto& device = devices_[record.device_id];
    check_duration_sum(record.device_id, device.total_duration_us, record.duration_us);

    const DeviceStats request{1, record.level == Level::Error ? 1u : 0u,
                              record.duration_us, record.duration_us};
    ++events_[record.event_code];
    add_totals(device, request);
    if (bucket_seconds_ > 0) {
        if (!bucket_totals) {
            bucket_totals = &buckets_[bucket_start];
        }
        add_totals(*bucket_totals, request);
    }
}

void Stats::merge(const Stats& other) {
    if (bucket_seconds_ != other.bucket_seconds_) {
        throw std::invalid_argument("cannot merge different time window widths");
    }
    // 先检查设备和时间窗口，再更新统计表。
    for (const auto& [id, incoming] : other.devices_) {
        const auto existing = devices_.find(id);
        if (existing != devices_.end()) {
            check_duration_sum(id, existing->second.total_duration_us, incoming.total_duration_us);
        }
    }
    for (const auto& [start, incoming] : other.buckets_) {
        const auto existing = buckets_.find(start);
        if (existing != buckets_.end()) {
            check_bucket_duration(start, existing->second.total_duration_us, incoming.total_duration_us);
        }
    }
    for (const auto& [id, incoming] : other.devices_) {
        add_totals(devices_[id], incoming);
    }
    for (const auto& [start, incoming] : other.buckets_) {
        add_totals(buckets_[start], incoming);
    }
    for (const auto& [event, count] : other.events_) {
        events_[event] += count;
    }
}

const Stats::DeviceTable& Stats::devices() const {
    return devices_;
}

const Stats::BucketTable& Stats::buckets() const {
    return buckets_;
}

std::int64_t Stats::bucket_seconds() const {
    return bucket_seconds_;
}

std::vector<Stats::EventCount> Stats::top_events(std::size_t limit) const {
    std::vector<EventCount> ranking(events_.begin(), events_.end());
    std::sort(ranking.begin(), ranking.end(), [](const EventCount& a, const EventCount& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first < b.first;
    });
    if (ranking.size() > limit) {
        ranking.resize(limit);
    }
    return ranking;
}

}  // namespace netlog
