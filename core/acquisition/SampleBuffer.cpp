#include "acquisition/SampleBuffer.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace escope {

// Round up to next power of two (C++20 std::bit_ceil)
static std::size_t next_pow2(std::size_t n) {
    return std::bit_ceil(n);
}

SampleBuffer::SampleBuffer(std::size_t capacity)
    : capacity_(next_pow2(capacity))
    , mask_(capacity_ - 1)
    , buf_(capacity_)
{
    if (capacity == 0) throw std::invalid_argument("SampleBuffer: capacity must be > 0");
}

void SampleBuffer::push(double timestamp_ns, float voltage) {
    std::size_t idx = head_.load(std::memory_order_relaxed) & mask_;
    buf_[idx] = {timestamp_ns, voltage};

    head_.fetch_add(1, std::memory_order_release);
    std::size_t prev_count = count_.load(std::memory_order_relaxed);
    if (prev_count < capacity_) {
        count_.fetch_add(1, std::memory_order_relaxed);
    } else {
        overflow_.store(true, std::memory_order_relaxed);
    }
}

void SampleBuffer::push_batch(const AnalogSample* samples, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        push(samples[i].timestamp_ns, samples[i].voltage);
    }
}

void SampleBuffer::snapshot(std::vector<AnalogSample>& out) const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    std::size_t cnt  = std::min(count_.load(std::memory_order_acquire), capacity_);
    std::size_t head = head_.load(std::memory_order_acquire);
    out.resize(cnt);

    if (cnt < capacity_) {
        // Buffer hasn't wrapped: data is 0..cnt-1
        for (std::size_t i = 0; i < cnt; ++i) {
            out[i] = buf_[i];
        }
    } else {
        // Buffer has wrapped: oldest sample is at head (mod mask)
        for (std::size_t i = 0; i < cnt; ++i) {
            out[i] = buf_[(head + i) & mask_];
        }
    }
}

std::size_t SampleBuffer::size() const noexcept {
    return std::min(count_.load(std::memory_order_relaxed), capacity_);
}

void SampleBuffer::clear() {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    head_.store(0, std::memory_order_relaxed);
    count_.store(0, std::memory_order_relaxed);
    overflow_.store(false, std::memory_order_relaxed);
}

std::pair<float, float> SampleBuffer::voltage_range() const {
    std::vector<AnalogSample> tmp;
    snapshot(tmp);
    if (tmp.empty()) return {0.f, 0.f};

    float vmin = tmp[0].voltage;
    float vmax = tmp[0].voltage;
    for (auto& s : tmp) {
        vmin = std::min(vmin, s.voltage);
        vmax = std::max(vmax, s.voltage);
    }
    return {vmin, vmax};
}

std::pair<double, double> SampleBuffer::time_range_ns() const {
    std::vector<AnalogSample> tmp;
    snapshot(tmp);
    if (tmp.empty()) return {0.0, 0.0};
    return {tmp.front().timestamp_ns, tmp.back().timestamp_ns};
}

} // namespace escope
