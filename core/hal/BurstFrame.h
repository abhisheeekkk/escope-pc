#pragma once
// Burst frame header from the STM32 firmware, and the clock that places bursts
// on the session timeline. Header-only so it can be unit tested.
//
// Frame (little-endian):
//   [0] 0xE7   [1] version (1 or 2)   [2..3] flags   [4..7] sample rate (Hz)
//   [8..11] number of samples   [12..15] trigger sample index   [16..19] sequence
//   [20..23] reserved
//   version 2 only: [24..31] t0_ns, the device time of sample 0
//   then number-of-samples raw bytes (bit n = channel Dn)
//
// The device does not sample while a burst is uploaded, so the time between
// bursts is not in the data. Version 2 carries the device clock (a 240 MHz timer
// that never stops) for sample 0, so the PC can place every burst exactly instead
// of guessing from when it happened to arrive over USB.

#include <cstddef>
#include <cstdint>

namespace escope {

constexpr uint8_t  BURST_MAGIC       = 0xE7;
constexpr uint32_t BURST_MAX_SAMPLES = 1u << 20;
constexpr std::size_t BURST_HDR_V1   = 24;
constexpr std::size_t BURST_HDR_V2   = 32;

struct BurstHeader {
    uint8_t  version       = 0;
    uint16_t flags         = 0;
    uint32_t rate          = 0;
    uint32_t nsamp         = 0;
    uint32_t trigger_index = 0;
    uint32_t seq           = 0;
    uint32_t reserved      = 0;
    bool     has_t0        = false;   ///< version 2: t0_ns is valid
    uint64_t t0_ns         = 0;       ///< device time of sample 0
    std::size_t header_size = 0;
};

enum class BurstParse { NeedMore, Bad, Ok };

inline uint32_t burst_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

/// Parse a header at the start of @p p (which must begin with the magic byte).
/// NeedMore = not enough bytes yet; Bad = not a valid header (false magic hit).
inline BurstParse parse_burst_header(const uint8_t* p, std::size_t avail, BurstHeader& h) {
    if (avail < BURST_HDR_V1) return BurstParse::NeedMore;
    if (p[0] != BURST_MAGIC) return BurstParse::Bad;

    h.version = p[1];
    if (h.version != 1 && h.version != 2) return BurstParse::Bad;
    h.header_size = h.version == 2 ? BURST_HDR_V2 : BURST_HDR_V1;

    h.flags         = uint16_t(p[2] | (p[3] << 8));
    h.rate          = burst_u32(p + 4);
    h.nsamp         = burst_u32(p + 8);
    h.trigger_index = burst_u32(p + 12);
    h.seq           = burst_u32(p + 16);
    h.reserved      = burst_u32(p + 20);
    if (h.rate == 0 || h.nsamp == 0 || h.nsamp > BURST_MAX_SAMPLES) return BurstParse::Bad;

    h.has_t0 = false;
    h.t0_ns  = 0;
    if (h.version == 2) {
        if (avail < BURST_HDR_V2) return BurstParse::NeedMore;
        uint64_t lo = burst_u32(p + 24), hi = burst_u32(p + 28);
        h.t0_ns  = lo | (hi << 32);
        h.has_t0 = true;
    }
    return BurstParse::Ok;
}

/// Places bursts on the session timeline (ns).
///  - Version 2: by the device clock, so the spacing between bursts is exact and
///    does not depend on USB or scheduling latency. The first burst is anchored
///    near the wall clock; after that only device time is used.
///  - Version 1 (no device clock): by arrival time, as before.
/// If the device clock goes backwards (the board was reset), the timeline
/// continues from the end of the previous burst instead of jumping back.
class BurstClock {
public:
    /// @param wall_ns  time since capture start when the burst finished arriving
    /// @return         session time of sample 0 of this burst
    double place(const BurstHeader& h, double wall_ns) {
        const double dur = double(h.nsamp) * 1e9 / double(h.rate);
        double t0;

        if (!h.has_t0) {
            t0 = wall_ns - dur;
            have_ref_ = false;                       // a later version 2 burst re-anchors
        } else {
            if (have_ref_) {
                t0 = session_ref_ + double(int64_t(h.t0_ns - dev_ref_));
                if (t0 < last_end_ - 1e6) have_ref_ = false;   // device clock went backwards
            }
            if (!have_ref_) {
                dev_ref_ = h.t0_ns;
                double anchor = wall_ns - dur;
                session_ref_ = (have_any_ && anchor < last_end_ + 1e6) ? last_end_ + 1e6
                                                                       : (anchor > 0 ? anchor : 0);
                have_ref_ = true;
                t0 = session_ref_;
            }
        }
        have_any_ = true;
        last_end_ = t0 + dur;
        return t0;
    }

    double last_end_ns() const { return last_end_; }

private:
    bool     have_ref_    = false;
    bool     have_any_    = false;
    uint64_t dev_ref_     = 0;
    double   session_ref_ = 0;
    double   last_end_    = 0;
};

} // namespace escope
