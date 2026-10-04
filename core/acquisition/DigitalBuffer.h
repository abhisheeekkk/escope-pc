#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <mutex>

namespace escope {

/// A single digital edge event.
struct DigitalEdge {
    double   timestamp_ns;  ///< Nanoseconds from capture start
    uint8_t  channel;       ///< Channel index (0–15)
    bool     rising;        ///< true = rising edge, false = falling edge
    /// True for the per-burst level snapshot a capture source pushes at the start
    /// of each burst: it records the line level, it is NOT a real transition, and
    /// the time since the previous burst was not observed.
    bool     snapshot = false;
};

/// Edge-list storage for digital channels.
///
/// Instead of storing one bit per sample (which at 100 MS/s = 12.5 MB/s per channel),
/// we store only transitions. A 10-second capture of a 1 kHz signal produces only
/// ~20,000 edges rather than 1 billion samples.
///
/// This is the correct representation for protocol decoding — decoders consume
/// edges, not raw level arrays.
class DigitalBuffer {
public:
    static constexpr uint8_t MAX_CHANNELS = 16;

    /// Hard cap on how many edges a single channel retains, purely as a
    /// memory safety net against unbounded growth on a very long capture.
    /// It is NOT what bounds per-frame paint cost -- edges_in_range()/
    /// level_at() binary-search straight to the visible window, so their
    /// cost depends on what's on screen, not on total history. Because of
    /// that, this cap can and should be generous: too small a value quietly
    /// truncates retained history for a dense/fast-toggling signal, which
    /// shows up as the waveform only filling part of the screen at wider
    /// time/div settings even though the capture ran long enough to cover
    /// it. Oldest edges are dropped first once the cap is hit.
    static constexpr std::size_t MAX_EDGES_PER_CHANNEL = 4000000;

    explicit DigitalBuffer(uint8_t num_channels = 8);

    // Non-copyable
    DigitalBuffer(const DigitalBuffer&)            = delete;
    DigitalBuffer& operator=(const DigitalBuffer&) = delete;

    /// Record a rising or falling edge on a channel.
    void push_edge(double timestamp_ns, uint8_t channel, bool rising);

    /// Push an entire edge batch (from hardware DMA block).
    void push_batch(const DigitalEdge* edges, std::size_t count);

    /// Discard all edges with timestamp_ns < cutoff_ns.
    ///
    /// A continuous/rolling capture keeps producing edges forever; without
    /// eviction the per-channel history (and every operation that scans it)
    /// grows without bound for as long as the capture runs. Call this
    /// periodically with a cutoff a bit before the oldest timestamp still
    /// needed on screen.
    void trim_before(double cutoff_ns);

    /// Reconstruct the logic level on @p channel at time @p timestamp_ns.
    /// Returns false if no edges before the timestamp (assumes initial low).
    bool level_at(uint8_t channel, double timestamp_ns) const;

    /// Get all edges for a given channel in time order.
    std::vector<DigitalEdge> edges_for_channel(uint8_t channel) const;

    /// The most recent @p count edges for @p channel (fewer if not that
    /// many exist yet). Unlike edges_for_channel(), this doesn't copy the
    /// entire per-channel history -- callers that only ever look at a
    /// handful of the newest edges (e.g. a live frequency/level readout)
    /// should use this instead.
    std::vector<DigitalEdge> last_edges(uint8_t channel, std::size_t count) const;

    /// Get all edges in time order (all channels).
    std::vector<DigitalEdge> all_edges() const;

    /// Get edges within a time window [t_start_ns, t_end_ns].
    std::vector<DigitalEdge> edges_in_range(double t_start_ns, double t_end_ns) const;

    /// Total edge count across all channels.
    std::size_t total_edges() const;

    std::size_t edge_count(uint8_t channel) const;

    uint8_t num_channels() const noexcept { return num_channels_; }

    void clear();

    /// Time range covered. Returns {0,0} if empty.
    std::pair<double, double> time_range_ns() const;

private:
    uint8_t                              num_channels_;
    std::vector<std::vector<DigitalEdge>> edges_;  ///< Per-channel edge lists
    mutable std::mutex                   mutex_;
};

} // namespace escope
