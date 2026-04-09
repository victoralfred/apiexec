#pragma once

#include "../core/vendor_adapter.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <string>

namespace apiexec {

constexpr int DEFAULT_PAGE_SIZE = 100;

// A batch of parsed JSON records from a paginated REST API.
struct JsonBatch {
    nlohmann::json records;  // JSON array of records from this page
    std::size_t count = 0;   // Number of records in this batch
};

// GenericRestAdapter — VendorAdapter strategy for cursor-paginated REST APIs.
//
// Expects the API to:
//   - Accept a page token as a query parameter or in the request body
//   - Return a JSON object with a data array and an optional next-page token
//
// Configuration JSON:
//   {
//     "base_url": "https://api.example.com/v1/data",
//     "auth_header": "Bearer <token>",
//     "data_field": "data",
//     "next_token_field": "next",
//     "page_param": "cursor",
//     "page_size": 100
//   }
class GenericRestAdapter : public VendorAdapter<JsonBatch> {
public:
    struct Config {
        std::string base_url;
        std::string auth_header;
        std::string data_field = "data";
        std::string next_token_field = "next";
        std::string page_param = "cursor";
        int page_size = DEFAULT_PAGE_SIZE;
    };

    explicit GenericRestAdapter(Config cfg);

    auto build_request(const Cursor& cursor) -> Request override;
    auto parse_response(const Response& resp, JsonBatch& out) -> bool override;
    auto next_cursor(const Cursor& current, const Response& resp) -> Cursor override;
    auto is_retryable(const Response& resp) -> bool override;
    auto retry_after(const Response& resp) -> std::optional<int> override;

    static auto from_json(const std::string& config_json) -> Config;

private:
    Config config_;
    nlohmann::json last_parsed_;  // cached parse for next_cursor
};

} // namespace apiexec
