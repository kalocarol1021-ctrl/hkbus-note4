#include "http_fetch.h"

#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "net.h"

namespace {
constexpr char kTag[] = "http";
constexpr size_t kMaxBody = 160 * 1024;

std::string HostOf(const std::string& url) {
    const size_t start = url.find("://");
    if (start == std::string::npos) return "";
    const size_t from = start + 3;
    const size_t end = url.find('/', from);
    return url.substr(from, end == std::string::npos ? std::string::npos : end - from);
}
}  // namespace

esp_err_t HttpFetcher::OnEvent(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<Ctx*>(evt->user_data);
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key != nullptr && strcasecmp(evt->header_key, "Date") == 0) {
                net::SetTimeFromHttpDate(evt->header_value);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (ctx != nullptr && ctx->body != nullptr && evt->data_len > 0) {
                if (ctx->body->size() + evt->data_len > kMaxBody) {
                    ctx->overflow = true;
                } else {
                    ctx->body->append(static_cast<const char*>(evt->data), evt->data_len);
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_http_client_handle_t HttpFetcher::ClientFor(const std::string& url, bool fresh) {
    const std::string host = HostOf(url);
    for (auto it = conns_.begin(); it != conns_.end(); ++it) {
        if (it->host != host) continue;
        if (!fresh) {
            esp_http_client_set_url(it->client, url.c_str());
            return it->client;
        }
        esp_http_client_cleanup(it->client);
        conns_.erase(it);
        break;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = OnEvent;
    cfg.user_data = &ctx_;
    cfg.keep_alive_enable = true;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.user_agent = "zectrix-note4-hkbus/1.0";
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client != nullptr) conns_.push_back({host, client});
    return client;
}

bool HttpFetcher::FetchOnce(const std::string& url, const std::string& post_body,
                            std::string* body, bool fresh) {
    esp_http_client_handle_t client = ClientFor(url, fresh);
    if (client == nullptr) return false;
    body->clear();
    ctx_.body = body;
    ctx_.overflow = false;
    if (post_body.empty()) {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
        esp_http_client_set_post_field(client, nullptr, 0);
        esp_http_client_delete_header(client, "Content-Type");
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_body.c_str(), static_cast<int>(post_body.size()));
    }
    const esp_err_t err = esp_http_client_perform(client);
    ctx_.body = nullptr;
    const int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "%s: %s", url.c_str(), esp_err_to_name(err));
        return false;
    }
    if (status != 200 || ctx_.overflow) {
        ESP_LOGW(kTag, "%s: HTTP %d%s", url.c_str(), status, ctx_.overflow ? " (too large)" : "");
        return false;
    }
    return !body->empty();
}

bool HttpFetcher::Fetch(const std::string& url, const std::string& post_body, std::string* body) {
    if (FetchOnce(url, post_body, body, false)) return true;
    // A kept-alive connection may have been closed by the server; retry once
    // on a fresh connection.
    return FetchOnce(url, post_body, body, true);
}

void HttpFetcher::CloseAll() {
    for (Conn& c : conns_) esp_http_client_cleanup(c.client);
    conns_.clear();
}
