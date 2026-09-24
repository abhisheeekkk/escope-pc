#pragma once
#include "decoders/base/IDecoder.h"

namespace escope {

class UartDecoder : public IDecoder {
public:
    std::string name()        const override { return "UART"; }
    std::string description() const override {
        return "Decodes UART/serial: TX and/or RX channels.";
    }

    std::vector<std::string> required_channels() const override {
        return {"TX", "RX"};
    }

    void configure(const std::vector<std::pair<std::string,std::string>>& params) override;

    std::vector<DecodedEvent> decode(const DigitalBuffer& buf,
                                     const std::vector<DecoderChannelMap>& channels) override;

private:
    uint32_t baud_    = 115200;
    uint8_t  data_bits_ = 8;
    bool     parity_  = false;
    bool     two_stop_ = false;
    bool     invert_  = false;  ///< Inverted logic (RS232)

    std::vector<DecodedEvent> decode_channel(const DigitalBuffer& buf,
                                              uint8_t ch_idx,
                                              const std::string& role) const;
};

} // namespace escope
