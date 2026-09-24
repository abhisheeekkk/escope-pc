#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <span>
#include <mutex>
#include <atomic>

namespace escope {

/// A single analog sample: voltage (V) at a given time (ns from capture start).
struct AnalogSample {
    double  timestamp_ns;   ///< Nanoseconds from capture start
    float   voltage;        ///< Voltage in Volts
};

/// Thread-safe ring buffer for analog samples on one channel.
///
/// Writers (acquisition thread) call push().
/// Readers (GUI / analysis thread) call snapshot() or iterate via view().
///
/// The buffer uses a power-of-two capacity so wrap-around is a bitmask op.
class SampleBuffer {
public:
    /// @param capacity  Number of samples to hold. Rounded up to next power of 2.
    explicit SampleBuffer(std::size_t capacity = 1024 * 1024);

    // Non-copyable, movable
    SampleBuffer(const SampleBuffer&)            = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;
    SampleBuffer(SampleBuffer&&)                 = default;
    SampleBuffer& operator=(SampleBuffer&&)      = default;

    /// Push a single sample. Lock-free on the write path.
    void push(double timestamp_ns, float voltage);

    /// Push a batch of samples efficiently.
    void push_batch(const AnalogSample* samples, std::size_t count);

    /// Take a snapshot of all currently available samples into @p out.
    /// Safe to call from any thread.
    void snapshot(std::vector<AnalogSample>& out) const;

    /// Returns the number of samples currently in the buffer.
    std::size_t size() const noexcept;

    /// Returns the total capacity.
    std::size_t capacity() const noexcept { return capacity_; }

    /// True if the buffer has wrapped around (older data overwritten).
    bool has_overflowed() const noexcept { return overflow_.load(std::memory_order_relaxed); }

    /// Clear all samples.
    void clear();

    /// Minimum and maximum voltage in the buffer (for auto-scaling).
    /// Returns {0,0} if empty.
    std::pair<float, float> voltage_range() const;

    /// Time range of samples in buffer. Returns {0,0} if empty.
    std::pair<double, double> time_range_ns() const;

private:
    std::size_t                 capacity_;
    std::size_t                 mask_;
    std::vector<AnalogSample>   buf_;
    std::atomic<std::size_t>    head_{0};   ///< Write index
    std::atomic<std::size_t>    count_{0};  ///< Valid sample count (saturates at capacity_)
    std::atomic<bool>           overflow_{false};
    mutable std::mutex          snapshot_mutex_;
};

} // namespace escope
