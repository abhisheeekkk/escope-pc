#include "session/EdgeCodec.h"
#include <array>
#include <cstring>

namespace escope::codec {

namespace {

// ---- CRC-32, slicing-by-8 -----------------------------------------------------------------
struct CrcTables {
    uint32_t t[8][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1U) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s)
                t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFU];
    }
};
const CrcTables& tables() { static const CrcTables k; return k; }

// ---- order-preserving double <-> uint64 ---------------------------------------------------
constexpr uint64_t SIGN = 0x8000000000000000ULL;
inline uint64_t to_ordered(double d) {
    uint64_t b; std::memcpy(&b, &d, 8);
    return (b & SIGN) ? ~b : (b | SIGN);
}
inline double from_ordered(uint64_t o) {
    const uint64_t b = (o & SIGN) ? (o & ~SIGN) : ~o;
    double d; std::memcpy(&d, &b, 8);
    return d;
}

inline void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80U) { out.push_back(static_cast<uint8_t>(v | 0x80U)); v >>= 7; }
    out.push_back(static_cast<uint8_t>(v));
}

inline bool get_varint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 70 && p < end; shift += 7) {
        const uint8_t b = *p++;
        v |= static_cast<uint64_t>(b & 0x7FU) << shift;
        if (!(b & 0x80U)) return true;
    }
    return false;                                  // ran off the end, or longer than 10 bytes
}

} // namespace

uint32_t crc32(const void* data, std::size_t len, uint32_t seed) {
    const auto& T = tables().t;
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~seed;
    while (len >= 8) {
        uint32_t a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= c;
        c = T[7][a & 0xFF] ^ T[6][(a >> 8) & 0xFF] ^ T[5][(a >> 16) & 0xFF] ^ T[4][a >> 24] ^
            T[3][b & 0xFF] ^ T[2][(b >> 8) & 0xFF] ^ T[1][(b >> 16) & 0xFF] ^ T[0][b >> 24];
        p += 8; len -= 8;
    }
    while (len--) c = T[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

void encode_block(const DigitalEdge* e, std::size_t n, std::vector<uint8_t>& out) {
    out.clear();
    const std::size_t bm = (n + 7) / 8;
    out.reserve(8 + n * 3 + 2 * bm);

    uint64_t prev = to_ordered(e[0].timestamp_ns);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(prev >> (8 * i)));
    for (std::size_t i = 1; i < n; ++i) {
        const uint64_t cur = to_ordered(e[i].timestamp_ns);
        put_varint(out, cur - prev);               // wraps harmlessly if a list is ever unsorted
        prev = cur;
    }

    const std::size_t base = out.size();
    out.resize(base + 2 * bm, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (e[i].rising)   out[base + i / 8]      |= static_cast<uint8_t>(1U << (i % 8));
        if (e[i].snapshot) out[base + bm + i / 8] |= static_cast<uint8_t>(1U << (i % 8));
    }
}

bool decode_block(const uint8_t* p, std::size_t len, std::size_t n, uint8_t channel,
                  std::vector<DigitalEdge>& out) {
    out.clear();
    if (n == 0 || len < 8) return false;
    const uint8_t* end = p + len;
    const std::size_t bm = (n + 7) / 8;

    uint64_t cur = 0;
    for (int i = 0; i < 8; ++i) cur |= static_cast<uint64_t>(p[i]) << (8 * i);
    p += 8;

    out.resize(n);
    out[0].timestamp_ns = from_ordered(cur);
    for (std::size_t i = 1; i < n; ++i) {
        uint64_t d;
        if (!get_varint(p, end, d)) return false;
        cur += d;
        out[i].timestamp_ns = from_ordered(cur);
    }
    if (static_cast<std::size_t>(end - p) != 2 * bm) return false;   // anything else is damage

    const uint8_t* rising   = p;
    const uint8_t* snapshot = p + bm;
    for (std::size_t i = 0; i < n; ++i) {
        out[i].channel  = channel;
        out[i].rising   = (rising[i / 8]   >> (i % 8)) & 1U;
        out[i].snapshot = (snapshot[i / 8] >> (i % 8)) & 1U;
    }
    return true;
}

} // namespace escope::codec
