#pragma once
#include "decoders/base/IDecoder.h"
namespace escope {

/// I2C decoder. Channel roles: "SCL" and "SDA".
///
/// Emits one DecodedEvent per START / repeated START / STOP condition, per
/// address byte and per data byte (ACK/NACK included in the label). Events
/// carry `value` = the raw 8-bit byte for address/data frames (for an address
/// frame that is the address byte including the R/W bit).
class I2CDecoder : public IDecoder {
public:
    std::string name()        const override { return "I2C"; }
    std::string description() const override { return "I2C protocol decoder (7-bit addressing)."; }
    std::vector<std::string> required_channels() const override;
    /// "glitch_ns": pulses shorter than this on SCL or SDA are ignored (default 100 ns).
    void configure(const std::vector<std::pair<std::string,std::string>>& params) override;
    std::vector<DecodedEvent> decode(const DigitalBuffer& buf,
                                     const std::vector<DecoderChannelMap>& channels) override;

    /// Short pulses removed from SCL/SDA by the last decode() call.
    std::size_t glitches_filtered() const { return glitches_; }

private:
    double      glitch_ns_ = 100.0;   ///< shorter than any legal pulse up to 1 MHz (FM+: 260 ns)
    std::size_t glitches_  = 0;
};
} // namespace escope
