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

    /// Estimate the baud rate of @p channel from its recent edges: in UART
    /// data the shortest gap between edges is one bit time. Returns 0 if
    /// there is not enough activity. Snapped to a standard rate if within 4%.
    static uint32_t detect_baud(const DigitalBuffer& buf, uint8_t channel);

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
