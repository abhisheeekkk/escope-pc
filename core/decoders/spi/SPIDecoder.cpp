#include "decoders/spi/SPIDecoder.h"
#include "decoders/base/DecoderRegistry.h"
namespace escope {
REGISTER_DECODER(SPIDecoder);
std::vector<std::string> SPIDecoder::required_channels() const {
    // TODO: Phase 3
    return {};
}
std::vector<DecodedEvent> SPIDecoder::decode(const DigitalBuffer&,
                                                    const std::vector<DecoderChannelMap>&) {
    // TODO: implement in Phase 3
    return {};
}
} // namespace escope
