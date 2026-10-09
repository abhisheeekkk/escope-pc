#pragma once
// Compact, lossless block codec for digital edge lists, plus the CRC-32 used to protect every block
// in a session file.
//
// A block holds up to a few tens of thousands of consecutive edges of ONE channel:
//   [8 bytes]  first timestamp (order-preserving form of the double's bits)
//   [varints]  difference to the previous timestamp, again in order-preserving integer form
//   [n/8 bytes] rising bit of each edge
//   [n/8 bytes] snapshot bit of each edge
// Because a double's bit pattern, mapped to the order-preserving form, grows with the value, the
// differences between neighbouring timestamps are small integers and fit in 2 to 4 bytes instead
// of 8. The mapping is exact, so decoding returns the very same doubles.

#include "acquisition/DigitalBuffer.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace escope::codec {

/// CRC-32 (IEEE 802.3, the zlib polynomial), slicing-by-8: roughly 2 GB/s per core.
uint32_t crc32(const void* data, std::size_t len, uint32_t seed = 0);

/// Append the edges to @p out (cleared first). @p n must be at least 1.
void encode_block(const DigitalEdge* edges, std::size_t n, std::vector<uint8_t>& out);

/// Decode @p n edges for @p channel from a payload produced by encode_block. Returns false when
/// the payload is malformed (truncated, trailing bytes or an over-long varint).
bool decode_block(const uint8_t* payload, std::size_t len, std::size_t n, uint8_t channel,
                  std::vector<DigitalEdge>& out);

} // namespace escope::codec
