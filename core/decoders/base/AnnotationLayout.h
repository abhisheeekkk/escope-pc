#pragma once
// Decides what to draw for decoded protocol events at the current zoom.
//
// The waveform shows decoded events on a lane under the channel: S, P, address and
// data boxes with hex values and A / N for ACK / NACK. How much detail fits depends
// on the zoom, so this works out the items for a view and leaves painting to the GUI:
//   - zoomed in: one item per event (S, address, each byte, P)
//   - zoomed out: a whole transfer becomes one summary bar (3C W, 36B)
//   - far out: neighbouring items closer than a couple of pixels merge into one bar
// No Qt here, so it can be unit tested.

#include "decoders/base/IDecoder.h"
#include <memory>
#include <string>
#include <vector>

namespace escope {

struct AnnotationItem {
    enum class Kind : uint8_t {
        Start, Repeated, Stop, Marker,      // zero-width markers: S, Sr, P, ~
        Address, Data, Error, Field,             // one frame
        Group,                              // a whole transfer, zoomed out
        Dense,                              // several items merged, far zoomed out
    };
    Kind        kind  = Kind::Data;
    double      start_ns = 0, end_ns = 0;
    std::string text;                       ///< short label: S, P, 3C W, 3E, 3C W . 36B
    std::string sub;                        ///< optional second label (UART character)
    std::string detail;                     ///< tooltip text
    int         ack   = -1;                 ///< 1 = ACK, 0 = NACK, -1 = not applicable
    bool        error = false;
    int         count = 1;                  ///< events folded into a Group or Dense item
    std::size_t first = 0;                  ///< index of the first event it covers
};

struct AnnotationView {
    double t0_ns = 0, t1_ns = 0;            ///< visible time range
    double px_per_ns = 0;                   ///< horizontal scale
    double group_px  = 150.0;               ///< a transfer narrower than this is drawn as one bar
    double min_byte_px = 22.0;              ///< per-byte boxes only if each is at least this wide (else one bar)
    double merge_px  = 1.5;                 ///< narrow items closer than this merge into a Dense bar
    double narrow_px = 3.0;                 ///< narrower than this an item can show no text and may merge
};

class AnnotationIndex {
public:
    /// Index a (time sorted) list of decoded events. Cheap to keep and query.
    void build(std::shared_ptr<const std::vector<DecodedEvent>> events);

    bool empty() const { return !ev_ || ev_->empty(); }
    const std::vector<DecodedEvent>* events() const { return ev_.get(); }

    /// Items to draw for @p view, sorted by start time.
    std::vector<AnnotationItem> items(const AnnotationView& view) const;

private:
    struct Span {                           // one transfer: START .. STOP
        std::size_t first, last;
        double      start_ns, end_ns;
        std::string summary;
        std::string detail;
        int         data_bytes = 0;
        bool        error = false;
    };
    std::shared_ptr<const std::vector<DecodedEvent>> ev_;
    std::vector<Span>        spans_;
    std::vector<std::size_t> loose_;        // events outside any transfer (e.g. UART frames)
};

/// Item for a single event (exposed for tests).
AnnotationItem make_annotation_item(const DecodedEvent& e, std::size_t index);

} // namespace escope
