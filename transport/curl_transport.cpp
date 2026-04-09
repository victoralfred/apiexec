#include "curl_transport.hpp"

#include <algorithm>
#include <cctype>
#include <curl/curl.h>

namespace apiexec {

// ---------- write callback for response body ----------

// Maximum response body size (16 MB). Exceeding this aborts the transfer.
inline constexpr size_t kMaxResponseBodySize = 16 * 1024 * 1024;

struct WriteContext {
    std::string* body;
    size_t limit;
};

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteContext*>(userdata);
    size_t total = size * nmemb;
    if (ctx->body->size() + total > ctx->limit) {
        return 0;  // Abort: causes CURLE_WRITE_ERROR
    }
    ctx->body->append(ptr, total);
    return total;
}

// ---------- header callback to capture response headers ----------

static size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    size_t total = size * nitems;
    std::string line(buffer, total);

    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);

        // Trim whitespace (index-based to avoid O(N^2) erase)
        auto trim = [](std::string& s) {
            auto start = s.find_first_not_of(" \t\r\n");
            auto end = s.find_last_not_of(" \t\r\n");
            s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };
        trim(key);
        trim(val);

        // Store key lowercase for case-insensitive lookup.
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        (*headers)[key] = val;
    }
    return total;
}

// ---------- progress callback for cancellation ----------

static int progress_cb(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                        curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* cancel = static_cast<std::atomic<bool>*>(clientp);
    return cancel->load(std::memory_order_relaxed) ? 1 : 0;
}

// ---------- CurlTransport ----------

CurlTransport::CurlTransport() {
    curl_handle_ = curl_easy_init();
}

CurlTransport::~CurlTransport() {
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
    }
}

CurlTransport::CurlTransport(CurlTransport&& other) noexcept
    : curl_handle_(other.curl_handle_) {
    other.curl_handle_ = nullptr;
}

CurlTransport& CurlTransport::operator=(CurlTransport&& other) noexcept {
    if (this != &other) {
        if (curl_handle_) curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = other.curl_handle_;
        other.curl_handle_ = nullptr;
    }
    return *this;
}

Response CurlTransport::execute(const Request& req, std::atomic<bool>& cancel_flag) {
    Response resp;

    if (!curl_handle_) {
        return resp;  // status_code 0 = network error
    }

    auto* curl = static_cast<CURL*>(curl_handle_);
    curl_easy_reset(curl);

    // URL
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());

    // Security: always verify TLS peer
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // Security: disable redirects to prevent SSRF and protocol downgrade attacks.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    // Timeouts
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    // TCP keepalive
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);

    // POST body
    if (req.method == Request::Method::POST) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }

    // Request headers
    struct curl_slist* slist = nullptr;
    for (const auto& [key, val] : req.headers) {
        std::string h = key + ": " + val;
        slist = curl_slist_append(slist, h.c_str());
    }
    if (slist) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
    }

    // Response body callback with size cap
    WriteContext write_ctx{&resp.body, kMaxResponseBodySize};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);

    // Response header callback
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);

    // Cancellation via progress callback
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel_flag);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // Execute
    CURLcode rc = curl_easy_perform(curl);

    if (slist) curl_slist_free_all(slist);

    if (rc == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        resp.status_code = static_cast<int32_t>(http_code);
    } else if (rc == CURLE_ABORTED_BY_CALLBACK) {
        resp.status_code = 0;  // cancelled
    } else {
        resp.status_code = 0;  // network error
    }

    return resp;
}

} // namespace apiexec
