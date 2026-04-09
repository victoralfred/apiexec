#include "default_policy.hpp"

#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace apiexec {
constexpr int MAX_CONFIG_SIZE = 65536; // 64KB
constexpr std::size_t MAX_PREFETCH_DEPTH = 4;
constexpr int MAX_RETRIES_LIMIT = 100;
static const std::set<std::string> kKnownKeys = {
    "max_retries", "base_backoff_ms",
    "window_grow_factor", "window_shrink_factor",
    "min_window_ms", "max_window_ms",
    "prefetch_depth"
};

auto DefaultPolicy::from_json(const std::string& config_json) -> DefaultPolicy {
    if (config_json.size() > MAX_CONFIG_SIZE) {
        throw std::invalid_argument("config_json exceeds 64KB limit");
    }
    auto j = nlohmann::json::parse(config_json);
    // Reject unknown keys
    for (const auto& [key, _] : j.items()) {
        if (kKnownKeys.find(key) == kKnownKeys.end()) {
            throw std::invalid_argument("unknown policy config key: " + key);
        }
    }
    Config cfg;
    cfg.max_retries         = j.value("max_retries", cfg.max_retries);
    cfg.base_backoff_ms     = j.value("base_backoff_ms", cfg.base_backoff_ms);
    cfg.window_grow_factor  = j.value("window_grow_factor", cfg.window_grow_factor);
    cfg.window_shrink_factor = j.value("window_shrink_factor", cfg.window_shrink_factor);
    cfg.min_window_ms       = j.value("min_window_ms", cfg.min_window_ms);
    cfg.max_window_ms       = j.value("max_window_ms", cfg.max_window_ms);
    cfg.prefetch_depth_val  = j.value("prefetch_depth", cfg.prefetch_depth_val);

    // Validate ranges
    if (cfg.max_retries < 0 || cfg.max_retries > MAX_RETRIES_LIMIT)
    {
            throw std::invalid_argument("max_retries must be in [0, " + std::to_string(MAX_RETRIES_LIMIT) + "]");
    }   
    if (cfg.base_backoff_ms <= 0){
        throw std::invalid_argument("base_backoff_ms must be positive");
    }
    if (cfg.window_grow_factor <= 1.0)
    {
        throw std::invalid_argument("window_grow_factor must be > 1.0");

    }    
    if (cfg.window_shrink_factor <= 0.0 || cfg.window_shrink_factor >= 1.0)
    {
        throw std::invalid_argument("window_shrink_factor must be in (0, 1)");

    }    
    if (cfg.min_window_ms <= 0)
    {
        throw std::invalid_argument("min_window_ms must be positive");

    }    
    if (cfg.max_window_ms < cfg.min_window_ms)
    {
        throw std::invalid_argument("max_window_ms must be >= min_window_ms");

    }    
    if (cfg.prefetch_depth_val > MAX_PREFETCH_DEPTH)
    {
        throw std::invalid_argument("prefetch_depth must be in [0, " + std::to_string(MAX_PREFETCH_DEPTH) + "]");
    }
    return DefaultPolicy(cfg);
}

} // namespace apiexec
