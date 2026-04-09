#include "registry.hpp"

namespace apiexec {

auto Registry::instance() -> Registry& {
    static Registry reg;
    return reg;
}

auto Registry::register_adapter(const std::string& name, AdapterFactory factory) -> void {
    factories_[name] = std::move(factory);
}

auto Registry::find(const std::string& name) const -> AdapterFactory {
    auto it = factories_.find(name);
    return it != factories_.end() ? it->second : nullptr;
}

auto Registry::has(const std::string& name) const -> bool {
    return factories_.find(name) != factories_.end();
}

auto Registry::adapter_names() const -> std::vector<std::string> {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    for (const auto& [name, _] : factories_) {
        names.push_back(name);
    }
    return names;
}

} // namespace apiexec
