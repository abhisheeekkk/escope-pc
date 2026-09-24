#include "decoders/base/DecoderRegistry.h"

namespace escope {

DecoderRegistry& DecoderRegistry::instance() {
    static DecoderRegistry inst;
    return inst;
}

void DecoderRegistry::register_decoder(const std::string& name, DecoderFactory factory) {
    factories_[name] = std::move(factory);
}

std::unique_ptr<IDecoder> DecoderRegistry::create(const std::string& name) const {
    auto it = factories_.find(name);
    if (it == factories_.end()) return nullptr;
    return it->second();
}

std::vector<std::string> DecoderRegistry::available() const {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    for (const auto& [k, _] : factories_) names.push_back(k);
    return names;
}

} // namespace escope
