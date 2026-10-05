#include "decoders/base/AnnotationLayout.h"
#include <algorithm>
#include <cstdio>

namespace escope {

namespace {
using Kind = AnnotationItem::Kind;

std::string hex2(int v) {
    char b[8];
    std::snprintf(b, sizeof b, "%02X", v & 0xFF);
    return b;
}

bool ends_with(const std::string& s, const char* suf) {
    const std::size_t n = std::char_traits<char>::length(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

int ack_of(const std::string& label) {
    if (ends_with(label, " NACK")) return 0;
    if (ends_with(label, " ACK"))  return 1;
    return -1;
}

std::string fmt_duration(double ns) {
    char b[32];
    if (ns >= 1e6)      std::snprintf(b, sizeof b, "%.3f ms", ns / 1e6);
    else if (ns >= 1e3) std::snprintf(b, sizeof b, "%.2f us", ns / 1e3);
    else                std::snprintf(b, sizeof b, "%.0f ns", ns);
    return b;
}

bool is_marker(Kind k) {
    return k == Kind::Start || k == Kind::Repeated || k == Kind::Stop || k == Kind::Marker;
}
} // namespace

AnnotationItem make_annotation_item(const DecodedEvent& e, std::size_t index) {
    using T = DecodedEvent::Type;
    AnnotationItem it;
    it.start_ns = e.start_ns;
    it.end_ns   = e.end_ns;
    it.first    = index;
    it.error    = e.is_error;
    it.detail   = e.detail.empty() ? e.label : e.detail;

    if (e.type == T::Control) {
        if      (e.label == "START") { it.kind = Kind::Start;    it.text = "S";  it.detail = "START condition"; }
        else if (e.label == "Sr")    { it.kind = Kind::Repeated; it.text = "Sr"; it.detail = "Repeated START"; }
        else if (e.label == "STOP")  { it.kind = Kind::Stop;     it.text = "P";  it.detail = "STOP condition"; }
        else                         { it.kind = Kind::Marker;   it.text = e.label;
                                       it.detail = e.detail.empty() ? "Capture began mid-transfer" : e.detail; }
        it.end_ns = it.start_ns;
        return it;
    }
    if (e.type == T::Error || e.is_error) {
        it.kind = Kind::Error;
        it.text = "!";
        it.error = true;
        return it;
    }
    it.ack = ack_of(e.label);
    if (e.type == T::Address) {
        it.kind = Kind::Address;
        it.text = hex2(e.value >> 1) + ((e.value & 1) ? " R" : " W");
        it.detail += it.ack == 1 ? "  (ACK)" : it.ack == 0 ? "  (NACK)" : "";
        return it;
    }
    it.kind = Kind::Data;
    if (e.value >= 0) {
        it.text = hex2(e.value);
        const bool uart = e.label.compare(0, 3, "TX ") == 0 || e.label.compare(0, 3, "RX ") == 0;
        if (uart) {
            if (e.value >= 32 && e.value < 127) it.sub = std::string(1, static_cast<char>(e.value));
            else if (e.value == 10) it.sub = "LF";
            else if (e.value == 13) it.sub = "CR";
        }
        if (it.ack == 1) it.detail += "  (ACK)";
        if (it.ack == 0) it.detail += "  (NACK)";
    } else {
        it.text = e.label;
    }
    return it;
}

void AnnotationIndex::build(std::shared_ptr<const std::vector<DecodedEvent>> events) {
    ev_ = std::move(events);
    spans_.clear();
    loose_.clear();
    if (!ev_) return;
    const auto& ev = *ev_;

    bool open = false;
    Span cur{};
    std::string addr_text;
    auto close = [&](std::size_t last) {
        cur.last     = last;
        cur.end_ns   = ev[last].type == DecodedEvent::Type::Control && ev[last].label == "STOP"
                       ? ev[last].start_ns : ev[last].end_ns;
        char b[64];
        std::snprintf(b, sizeof b, " . %dB", cur.data_bytes);
        cur.summary = (addr_text.empty() ? std::string("~") : addr_text) + b;
        cur.detail  = "Transfer " + (addr_text.empty() ? std::string("(began mid-transfer)") : addr_text) +
                      ", " + std::to_string(cur.data_bytes) + " data bytes, " +
                      fmt_duration(cur.end_ns - cur.start_ns);
        spans_.push_back(cur);
        open = false;
    };

    for (std::size_t i = 0; i < ev.size(); ++i) {
        const AnnotationItem it = make_annotation_item(ev[i], i);
        const Kind k = it.kind;
        if (k == Kind::Start || k == Kind::Marker || k == Kind::Repeated) {
            if (open && i > 0) close(i - 1);
            open = true;
            cur = Span{};
            cur.first = i;
            cur.start_ns = ev[i].start_ns;
            addr_text.clear();
        } else if (k == Kind::Stop) {
            if (open) close(i); else loose_.push_back(i);
        } else if (open) {
            if (k == Kind::Address) addr_text = it.text;
            else if (k == Kind::Data) ++cur.data_bytes;
            if (it.error) cur.error = true;
        } else {
            loose_.push_back(i);
        }
    }
    if (open && !ev.empty() && ev.size() - 1 >= cur.first) close(ev.size() - 1);
}

std::vector<AnnotationItem> AnnotationIndex::items(const AnnotationView& v) const {
    std::vector<AnnotationItem> out;
    if (empty() || v.px_per_ns <= 0) return out;
    const auto& ev = *ev_;

    // Transfers overlapping the view (spans are in time order).
    auto sit = std::lower_bound(spans_.begin(), spans_.end(), v.t0_ns,
        [](const Span& s, double t) { return s.end_ns < t; });
    for (; sit != spans_.end() && sit->start_ns <= v.t1_ns; ++sit) {
        const double wpx = (sit->end_ns - sit->start_ns) * v.px_per_ns;
        // Draw the bytes only if there is room to show them; otherwise one summary bar.
        const double per_byte_px = wpx / (sit->data_bytes + 1);
        if (wpx < v.group_px || per_byte_px < v.min_byte_px) {
            AnnotationItem g;
            g.kind     = Kind::Group;
            g.start_ns = sit->start_ns;
            g.end_ns   = sit->end_ns;
            g.text     = sit->summary;
            g.detail   = sit->detail;
            g.error    = sit->error;
            g.count    = sit->data_bytes + 1;
            g.first    = sit->first;
            out.push_back(std::move(g));
        } else {
            for (std::size_t j = sit->first; j <= sit->last; ++j) {
                if (ev[j].end_ns < v.t0_ns || ev[j].start_ns > v.t1_ns) continue;
                out.push_back(make_annotation_item(ev[j], j));
            }
        }
    }

    // Frames that belong to no transfer (UART, stray bytes).
    auto lit = std::lower_bound(loose_.begin(), loose_.end(), v.t0_ns,
        [&](std::size_t i, double t) { return ev[i].end_ns < t; });
    for (; lit != loose_.end() && ev[*lit].start_ns <= v.t1_ns; ++lit)
        out.push_back(make_annotation_item(ev[*lit], *lit));

    std::stable_sort(out.begin(), out.end(),
        [](const AnnotationItem& a, const AnnotationItem& b) { return a.start_ns < b.start_ns; });

    // Far zoomed out: fold items that would be less than merge_px apart into one bar.
    // Only items too narrow to show anything merge: byte boxes that touch each other
    // while zoomed in are real, separate frames and must stay separate.
    const double gap_ns = v.merge_px / v.px_per_ns;
    auto narrow = [&](const AnnotationItem& i) { return (i.end_ns - i.start_ns) * v.px_per_ns < v.narrow_px; };
    std::vector<AnnotationItem> merged;
    merged.reserve(out.size());
    for (auto& it : out) {
        if (!merged.empty() && !is_marker(it.kind) && !is_marker(merged.back().kind) &&
            it.start_ns - merged.back().end_ns < gap_ns &&
            narrow(it) && (merged.back().kind == Kind::Dense || narrow(merged.back()))) {
            AnnotationItem& m = merged.back();
            if (m.kind != Kind::Dense) {
                m.kind   = Kind::Dense;
                m.detail = "";
            }
            m.end_ns = std::max(m.end_ns, it.end_ns);
            m.count += it.count;
            m.error  = m.error || it.error;
            m.text.clear();
            m.sub.clear();
            m.ack = -1;
        } else {
            merged.push_back(std::move(it));
        }
    }
    for (auto& m : merged)
        if (m.kind == Kind::Dense) m.detail = std::to_string(m.count) + " items (zoom in for detail)";
    return merged;
}

} // namespace escope
