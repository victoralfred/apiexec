#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace apiexec {

// AdapterFactory creates an opaque engine handle from a JSON config string.
// Returns nullptr on failure.
using AdapterFactory = std::function<void*(const std::string& config_json)>;

class Registry {
public:
    static Registry& instance();

    void register_adapter(const std::string& name, AdapterFactory factory);
    AdapterFactory find(const std::string& name) const;
    std::vector<std::string> adapter_names() const;

private:
    Registry() = default;
    std::unordered_map<std::string, AdapterFactory> factories_;
};

#define APIEXEC_REGISTER_ADAPTER(name, factory_fn)                          \
    static bool _apiexec_reg_##name = [] {                                  \
        ::apiexec::Registry::instance().register_adapter(#name, factory_fn);\
        return true;                                                        \
    }()

} // namespace apiexec
