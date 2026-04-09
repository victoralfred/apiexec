#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace apiexec {

// AdapterFactory creates an adapter instance from a JSON config string.
// Returns nullptr (as void*) on failure. The caller takes ownership.
using AdapterFactory = std::function<void*(const std::string& config_json)>;

class Registry {
public:
    static auto instance() -> Registry&;

    auto register_adapter(const std::string& name, AdapterFactory factory) -> void;
    auto find(const std::string& name) const -> AdapterFactory;
    auto adapter_names() const -> std::vector<std::string>;
    auto has(const std::string& name) const -> bool;

private:
    Registry() = default;
    std::unordered_map<std::string, AdapterFactory> factories_;
};

// Static self-registration macro. Use at file scope in each adapter's .cpp.
#define APIEXEC_REGISTER_ADAPTER(name, factory_fn)                          \
    static const bool _apiexec_reg_##name = [] {                            \
        ::apiexec::Registry::instance().register_adapter(#name, factory_fn);\
        return true;                                                        \
    }()

} // namespace apiexec
