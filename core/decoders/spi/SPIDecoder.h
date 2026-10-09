#pragma once
#include "decoders/base/IDecoder.h"

namespace escope {

/// SPI decoder for a master with up to five logic lines.
///
/// Channel roles: "CLK" (required), "MOSI" and/or "MISO" (at least one), "CS" (optional,
/// chip select) and "DC" (optional: the data/command line of a display, low = command).
///
/// Each clocked word (8 bits by default, MSB first) becomes one event on the data line it
/// was read from. With DC assigned, a MOSI word sent while DC is low is a command
/// (Type::Address, "CMD 0x2A"), otherwise a data word ("MOSI 0x00", "MISO 0xFF"). With CS
/// assigned, each chip-select window is a transaction (Control events "CS" and "/CS");
/// without it, a long pause in the clock ends a transaction. Words cut short by the end of
/// a capture burst or by chip select going inactive are reported as errors.
///
/// The clock polarity is detected from the idle level of CLK, and the clock phase from where
/// the data lines change relative to the clock, so no mode needs to be entered. That needs
/// at least about four capture samples per clock half period. Both can be forced with the
/// "mode" parameter.
class SPIDecoder : public IDecoder {
public:
    std::string name()        const override { return "SPI"; }
    std::string description() const override {
        return "SPI decoder: CLK, MOSI and/or MISO, optional CS and DC; mode detected from the signals.";
    }
    std::vector<std::string> required_channels() const override { return {"CLK", "MOSI"}; }

    /// "mode" (0-3, absent or "auto" = detect), "bits" (word size 1-16, default 8),
    /// "lsb" ("1" = least significant bit first), "dc_active" ("high" = DC high means command),
    /// "cs_active" ("high" = chip select is active high; default active low).
    void configure(const std::vector<std::pair<std::string,std::string>>& params) override;

    std::vector<DecodedEvent> decode(const DigitalBuffer& buf,
                                     const std::vector<DecoderChannelMap>& channels) override;

    int         mode_used()    const { return mode_used_; }
    bool        mode_assumed() const { return mode_assumed_; }   ///< no data edges to infer the phase from
    double      clock_hz()     const { return clock_hz_; }       ///< 0 if unknown
    std::size_t words()        const { return words_; }
    std::size_t errors()       const { return errors_; }

private:
    int  mode_cfg_ = -1;
    int  bits_     = 8;
    bool lsb_      = false;
    bool dc_cmd_high_ = false;
    bool cs_high_ = false;
    int         mode_used_ = 0;
    bool        mode_assumed_ = false;
    double      clock_hz_ = 0;
    std::size_t words_ = 0, errors_ = 0;
};

} // namespace escope
