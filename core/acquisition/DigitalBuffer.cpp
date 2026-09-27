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

namespace {
void cap_channel(std::vector<DigitalEdge>& ch) {
    if (ch.size() > DigitalBuffer::MAX_EDGES_PER_CHANNEL)
        ch.erase(ch.begin(), ch.end() - DigitalBuffer::MAX_EDGES_PER_CHANNEL);
}
} // namespace

void DigitalBuffer::push_edge(double timestamp_ns, uint8_t channel, bool rising) {
    if (channel >= num_channels_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& ch = edges_[channel];
    ch.push_back({timestamp_ns, channel, rising});
    cap_channel(ch);
}

void DigitalBuffer::push_batch(const DigitalEdge* edges, std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < count; ++i) {
        if (edges[i].channel < num_channels_) {
            edges_[edges[i].channel].push_back(edges[i]);
        }
    }
    for (auto& ch : edges_) cap_channel(ch);
}

bool DigitalBuffer::level_at(uint8_t channel, double timestamp_ns) const {
    if (channel >= num_channels_) return false;
    std::lock_guard<std::mutex> lock(mutex_);

    // Edges are pushed in increasing timestamp order, so the last edge at or
    // before timestamp_ns can be found with a binary search instead of a
    // linear scan of the whole (potentially huge) per-channel history.
    const auto& ch = edges_[channel];
    auto it = std::upper_bound(ch.begin(), ch.end(), timestamp_ns,
        [](double t, const DigitalEdge& e) { return t < e.timestamp_ns; });
    if (it == ch.begin()) return false; // assume starts low
    return (it - 1)->rising;
}

std::vector<DigitalEdge> DigitalBuffer::edges_for_channel(uint8_t channel) const {
    if (channel >= num_channels_) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    return edges_[channel];
}

std::vector<DigitalEdge> DigitalBuffer::last_edges(uint8_t channel, std::size_t count) const {
    if (channel >= num_channels_) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& ch = edges_[channel];
    std::size_t n = std::min(count, ch.size());
    return std::vector<DigitalEdge>(ch.end() - n, ch.end());
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
    // Each per-channel vector is already time-sorted, so binary search
    // straight to the matching sub-range instead of scanning every edge
    // ever recorded -- important once a capture has accumulated far more
    // history than fits in the current view.
    for (const auto& ch : edges_) {
        auto lo = std::lower_bound(ch.begin(), ch.end(), t_start_ns,
            [](const DigitalEdge& e, double t) { return e.timestamp_ns < t; });
        auto hi = std::upper_bound(ch.begin(), ch.end(), t_end_ns,
            [](double t, const DigitalEdge& e) { return t < e.timestamp_ns; });
        result.insert(result.end(), lo, hi);
    }
    std::sort(result.begin(), result.end(),
        [](const DigitalEdge& a, const DigitalEdge& b) {
            return a.timestamp_ns < b.timestamp_ns;
        });
    return result;
}

void DigitalBuffer::trim_before(double cutoff_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& ch : edges_) {
        auto cut = std::lower_bound(ch.begin(), ch.end(), cutoff_ns,
            [](const DigitalEdge& e, double t) { return e.timestamp_ns < t; });
        if (cut != ch.begin())
            ch.erase(ch.begin(), cut);
    }
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
    // Each channel's edges are pushed in increasing timestamp order, so its
    // earliest/latest timestamp is just its first/last element -- no need
    // to scan every edge. This function is called from paintGL every frame
    // while following live data, so with millions of retained edges the old
    // full scan turned into a constant, visible per-frame stall.
    double tmin = 1e18, tmax = -1e18;
    bool found = false;
    for (const auto& ch : edges_) {
        if (ch.empty()) continue;
        tmin = std::min(tmin, ch.front().timestamp_ns);
        tmax = std::max(tmax, ch.back().timestamp_ns);
        found = true;
    }
    if (!found) return {0.0, 0.0};
    return {tmin, tmax};
}

} // namespace escope
