// Fuzz targets for adapter parse_response() paths.
//
// Build with: clang++ -g -fsanitize=fuzzer,address -std=c++17 \
//   -I../../source adapter_parse_fuzz.cpp \
//   -L../../build -lapiexec_adapters -lapiexec_policy -lnlohmann_json -lcurl -lstdc++
//
// Or via CMake with FUZZ=ON option (future).

#include "adapters/generic_rest.hpp"
#include "adapters/datadog_metrics.hpp"
#include "adapters/openai.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <cstring>
#include <string>

using namespace apiexec;

// --- Fuzz target: GenericRestAdapter::parse_response ---

static GenericRestAdapter make_rest_adapter() {
    GenericRestAdapter::Config cfg;
    cfg.base_url = "http://fuzz";
    return GenericRestAdapter(cfg);
}

static DatadogMetricsAdapter make_dd_adapter() {
    DatadogMetricsAdapter::Config cfg;
    cfg.api_key = "fuzz";
    cfg.app_key = "fuzz";
    cfg.query = "fuzz";
    return DatadogMetricsAdapter(cfg);
}

static OpenAIAdapter make_oai_adapter() {
    OpenAIAdapter::Config cfg;
    cfg.api_key = "fuzz";
    cfg.prompts = {"fuzz"};
    return OpenAIAdapter(cfg);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap input size to prevent OOM
    constexpr size_t kMaxInputSize = 64 * 1024;
    if (size > kMaxInputSize) return 0;

    std::string body(reinterpret_cast<const char*>(data), size);
    Response resp{200, body, {}};

    // Fuzz all three adapters' parse paths
    {
        auto adapter = make_rest_adapter();
        JsonBatch out;
        adapter.parse_response(resp, out);
    }
    {
        auto adapter = make_dd_adapter();
        JsonBatch out;
        adapter.parse_response(resp, out);
    }
    {
        auto adapter = make_oai_adapter();
        JsonBatch out;
        adapter.parse_response(resp, out);
    }

    return 0;
}
