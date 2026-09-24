#pragma once
#include "decoders/base/IDecoder.h"
namespace escope {
class SPIDecoder : public IDecoder {
public:
    std::string name()        const override { return "SPI"; }
    std::string description() const override { return "SPI protocol decoder (stub - Phase 3)."; }
    std::vector<std::string> required_channels() const override;
    void configure(const std::vector<std::pair<std::string,std::string>>& params) override {}
    std::vector<DecodedEvent> decode(const DigitalBuffer& buf,
                                     const std::vector<DecoderChannelMap>& channels) override;
};
} // namespace escope
