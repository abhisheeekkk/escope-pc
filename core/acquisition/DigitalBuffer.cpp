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
void cap_channel(std::deque<DigitalEdge>& ch) {
    if (ch.size() > DigitalBuffer::MAX_EDGES_PER_CHANNEL)
        ch.erase(ch.begin(), ch.end() - static_cast<std::ptrdiff_t>(DigitalBuffer::MAX_EDGES_PER_CHANNEL));
}
} // namespace

void DigitalBuffer::push_edge(double timestamp_ns, uint8_t channel, bool rising) {
    if (channel >= num_channels_ || !((store_mask() >> channel) & 1U)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& ch = edges_[channel];
    ch.push_back({timestamp_ns, channel, rising});
    cap_channel(ch);
    version_.fetch_add(1, std::memory_order_relaxed);
}

void DigitalBuffer::push_batch(const DigitalEdge* edges, std::size_t count) {
    const uint8_t mask = store_mask();
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < count; ++i) {
        if (edges[i].channel < num_channels_ && ((mask >> edges[i].channel) & 1U)) {
            edges_[edges[i].channel].push_back(edges[i]);
        }
    }
    for (auto& ch : edges_) cap_channel(ch);
    version_.fetch_add(1, std::memory_order_relaxed);
}

void DigitalBuffer::mark_burst_end(double timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    burst_ends_.push_back(timestamp_ns);
    version_.fetch_add(1, std::memory_order_relaxed);
}

void DigitalBuffer::assign_channel(uint8_t channel, std::deque<DigitalEdge>&& edges) {
    if (channel >= num_channels_) return;
    cap_channel(edges);
    std::lock_guard<std::mutex> lock(mutex_);
    edges_[channel] = std::move(edges);
    version_.fetch_add(1, std::memory_order_relaxed);
}

void DigitalBuffer::set_burst_ends(std::vector<double> ends) {
    std::lock_guard<std::mutex> lock(mutex_);
    burst_ends_ = std::move(ends);
    version_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<double> DigitalBuffer::burst_ends() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return burst_ends_;
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
    return std::vector<DigitalEdge>(edges_[channel].begin(), edges_[channel].end());
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

void DigitalBuffer::columns(uint8_t channel, double t0, double t1, int ncols,
                            std::vector<EdgeColumn>& out) const {
    out.clear();
    if (channel >= num_channels_ || ncols <= 0 || t1 <= t0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& ch = edges_[channel];
    const double step = (t1 - t0) / ncols;
    auto less_t = [](const DigitalEdge& e, double t) { return e.timestamp_ns < t; };
    auto it = std::lower_bound(ch.begin(), ch.end(), t0, less_t);
    while (it != ch.end() && it->timestamp_ns < t1) {
        const int c = std::clamp(static_cast<int>((it->timestamp_ns - t0) / step), 0, ncols - 1);
        auto nxt = std::lower_bound(it, ch.end(), t0 + (c + 1) * step, less_t);
        if (nxt == it) ++nxt;                                   // guard against rounding at a boundary
        out.push_back({it->timestamp_ns, static_cast<uint32_t>(nxt - it), (nxt - 1)->rising});
        it = nxt;
    }
}

double DigitalBuffer::high_time_ns(uint8_t channel, double t0, double t1) const {
    if (channel >= num_channels_ || t1 <= t0) return 0.0;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& ch = edges_[channel];
    auto lo = std::upper_bound(ch.begin(), ch.end(), t0,
        [](double t, const DigitalEdge& e) { return t < e.timestamp_ns; });
    bool   high = lo != ch.begin() && (lo - 1)->rising;
    double t    = t0, total = 0.0;
    for (auto it = lo; it != ch.end() && it->timestamp_ns < t1; ++it) {
        if (high) total += it->timestamp_ns - t;
        t = it->timestamp_ns;
        high = it->rising;
    }
    if (high) total += t1 - t;
    return total;
}

void DigitalBuffer::trim_before(double cutoff_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& ch : edges_) {
        auto cut = std::lower_bound(ch.begin(), ch.end(), cutoff_ns,
            [](const DigitalEdge& e, double t) { return e.timestamp_ns < t; });
        if (cut != ch.begin())
            ch.erase(ch.begin(), cut);
    }
    burst_ends_.erase(burst_ends_.begin(),
        std::lower_bound(burst_ends_.begin(), burst_ends_.end(), cutoff_ns));
    version_.fetch_add(1, std::memory_order_relaxed);
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

void DigitalBuffer::mark_trigger(double timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_trigger_ns_ = timestamp_ns;
}

double DigitalBuffer::last_trigger_ns() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_trigger_ns_;
}

void DigitalBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& ch : edges_) ch.clear();
    burst_ends_.clear();
    last_trigger_ns_ = -1.0;
    version_.fetch_add(1, std::memory_order_relaxed);
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
