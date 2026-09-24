#pragma once

#include "decoders/base/IDecoder.h"
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace escope {

using DecoderFactory = std::function<std::unique_ptr<IDecoder>()>;

/// Singleton registry for protocol decoders.
/// Decoders self-register at static initialisation time.
class DecoderRegistry {
public:
    static DecoderRegistry& instance();

    /// Register a decoder factory under @p name.
    void register_decoder(const std::string& name, DecoderFactory factory);

    /// Create an instance of decoder @p name. Returns nullptr if not found.
    std::unique_ptr<IDecoder> create(const std::string& name) const;

    /// List all registered decoder names.
    std::vector<std::string> available() const;

private:
    DecoderRegistry() = default;
    std::map<std::string, DecoderFactory> factories_;
};

/// RAII registration helper. Use REGISTER_DECODER macro.
struct DecoderRegistrar {
    DecoderRegistrar(const std::string& name, DecoderFactory f) {
        DecoderRegistry::instance().register_decoder(name, std::move(f));
    }
};

#define REGISTER_DECODER(ClassName) \
    static escope::DecoderRegistrar _reg_##ClassName( \
        ClassName{}.name(), []{ return std::make_unique<ClassName>(); })

} // namespace escope
