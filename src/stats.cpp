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

}  // namespace

void Stats::add(const Record& record) {
    auto& device = devices_[record.device_id];
    check_duration_sum(record.device_id, device.total_duration_us, record.duration_us);

    ++events_[record.event_code];
    ++device.requests;
    if (record.level == Level::Error) {
        ++device.errors;
    }
    device.total_duration_us += record.duration_us;
    device.max_duration_us = std::max(device.max_duration_us, record.duration_us);
}

void Stats::merge(const Stats& other) {
    // 先检查所有设备，避免溢出时只合并了一部分统计。
    for (const auto& [id, incoming] : other.devices_) {
        const auto existing = devices_.find(id);
        if (existing != devices_.end()) {
            check_duration_sum(id, existing->second.total_duration_us, incoming.total_duration_us);
        }
    }
    for (const auto& [id, incoming] : other.devices_) {
        auto& device = devices_[id];
        device.requests += incoming.requests;
        device.errors += incoming.errors;
        device.total_duration_us += incoming.total_duration_us;
        device.max_duration_us = std::max(device.max_duration_us, incoming.max_duration_us);
    }
    for (const auto& [event, count] : other.events_) {
        events_[event] += count;
    }
}

const Stats::DeviceTable& Stats::devices() const {
    return devices_;
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
