#pragma once
#include "decoders/base/IDecoder.h"
namespace escope {
class I2CDecoder : public IDecoder {
public:
    std::string name()        const override { return "I2C"; }
    std::string description() const override { return "I2C protocol decoder (stub - Phase 3)."; }
    std::vector<std::string> required_channels() const override;
    void configure(const std::vector<std::pair<std::string,std::string>>& params) override {}
    std::vector<DecodedEvent> decode(const DigitalBuffer& buf,
                                     const std::vector<DecoderChannelMap>& channels) override;
};
} // namespace escope
