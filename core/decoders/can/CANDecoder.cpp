#include "decoders/can/CANDecoder.h"
#include "decoders/base/DecoderRegistry.h"
namespace escope {
REGISTER_DECODER(CANDecoder);
std::vector<std::string> CANDecoder::required_channels() const {
    // TODO: Phase 3
    return {};
}
std::vector<DecodedEvent> CANDecoder::decode(const DigitalBuffer&,
                                                    const std::vector<DecoderChannelMap>&) {
    // TODO: implement in Phase 3
    return {};
}
} // namespace escope
