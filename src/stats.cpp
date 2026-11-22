#include "netlog/stats.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace netlog {

void Stats::add(const Record& record) {
    auto& device = devices_[record.device_id];
    if (record.duration_us > std::numeric_limits<std::uint64_t>::max()
                             - device.total_duration_us) {
        throw std::overflow_error("total duration for device " + record.device_id
                                  + " exceeds uint64 range");
    }

    ++events_[record.event_code];
    ++device.requests;
    if (record.level == Level::Error) {
        ++device.errors;
    }
    device.total_duration_us += record.duration_us;
    device.max_duration_us = std::max(device.max_duration_us, record.duration_us);
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
