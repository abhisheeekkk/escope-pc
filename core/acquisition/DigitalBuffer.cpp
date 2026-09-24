#include "acquisition/DigitalBuffer.h"

#include <algorithm>
#include <stdexcept>

namespace escope {

DigitalBuffer::DigitalBuffer(uint8_t num_channels)
    : num_channels_(num_channels)
    , edges_(num_channels)
{
    if (num_channels == 0 || num_channels > MAX_CHANNELS) {
        throw std::invalid_argument("DigitalBuffer: num_channels must be 1–16");
    }
}

void DigitalBuffer::push_edge(double timestamp_ns, uint8_t channel, bool rising) {
    if (channel >= num_channels_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    edges_[channel].push_back({timestamp_ns, channel, rising});
}

void DigitalBuffer::push_batch(const DigitalEdge* edges, std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < count; ++i) {
        if (edges[i].channel < num_channels_) {
            edges_[edges[i].channel].push_back(edges[i]);
        }
    }
}

bool DigitalBuffer::level_at(uint8_t channel, double timestamp_ns) const {
    if (channel >= num_channels_) return false;
    std::lock_guard<std::mutex> lock(mutex_);

    const auto& ch = edges_[channel];
    // Find the last edge before or at timestamp_ns
    bool level = false; // assume starts low
    for (const auto& e : ch) {
        if (e.timestamp_ns <= timestamp_ns) {
            level = e.rising;
        } else {
            break;
        }
    }
    return level;
}

std::vector<DigitalEdge> DigitalBuffer::edges_for_channel(uint8_t channel) const {
    if (channel >= num_channels_) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    return edges_[channel];
}

std::vector<DigitalEdge> DigitalBuffer::all_edges() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DigitalEdge> result;
    for (const auto& ch : edges_) {
        result.insert(result.end(), ch.begin(), ch.end());
    }
    std::sort(result.begin(), result.end(),
        [](const DigitalEdge& a, const DigitalEdge& b) {
            return a.timestamp_ns < b.timestamp_ns;
        });
    return result;
}

std::vector<DigitalEdge> DigitalBuffer::edges_in_range(double t_start_ns, double t_end_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DigitalEdge> result;
    for (const auto& ch : edges_) {
        for (const auto& e : ch) {
            if (e.timestamp_ns >= t_start_ns && e.timestamp_ns <= t_end_ns) {
                result.push_back(e);
            }
        }
    }
    std::sort(result.begin(), result.end(),
        [](const DigitalEdge& a, const DigitalEdge& b) {
            return a.timestamp_ns < b.timestamp_ns;
        });
    return result;
}

std::size_t DigitalBuffer::total_edges() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto& ch : edges_) total += ch.size();
    return total;
}

std::size_t DigitalBuffer::edge_count(uint8_t channel) const {
    if (channel >= num_channels_) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return edges_[channel].size();
}

void DigitalBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& ch : edges_) ch.clear();
}

std::pair<double, double> DigitalBuffer::time_range_ns() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double tmin = 1e18, tmax = -1e18;
    bool found = false;
    for (const auto& ch : edges_) {
        for (const auto& e : ch) {
            tmin = std::min(tmin, e.timestamp_ns);
            tmax = std::max(tmax, e.timestamp_ns);
            found = true;
        }
    }
    if (!found) return {0.0, 0.0};
    return {tmin, tmax};
}

} // namespace escope
