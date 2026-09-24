#include "decoders/i2c/I2CDecoder.h"
#include "decoders/base/DecoderRegistry.h"
namespace escope {
REGISTER_DECODER(I2CDecoder);
std::vector<std::string> I2CDecoder::required_channels() const {
    // TODO: Phase 3
    return {};
}
std::vector<DecodedEvent> I2CDecoder::decode(const DigitalBuffer&,
                                                    const std::vector<DecoderChannelMap>&) {
    // TODO: implement in Phase 3
    return {};
}
} // namespace escope
