#include "registry.hpp"

namespace apiexec {

Registry& Registry::instance() {
    static Registry reg;
    return reg;
}

void Registry::register_adapter(const std::string& name, AdapterFactory factory) {
    factories_[name] = std::move(factory);
}

AdapterFactory Registry::find(const std::string& name) const {
    auto it = factories_.find(name);
    return it != factories_.end() ? it->second : nullptr;
}

std::vector<std::string> Registry::adapter_names() const {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    for (const auto& [name, _] : factories_) {
        names.push_back(name);
    }
    return names;
}

} // namespace apiexec
