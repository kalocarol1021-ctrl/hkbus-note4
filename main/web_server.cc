#include "web_server.h"

#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "net.h"

extern "C" {
extern const char kIndexHtmlStart[] asm("_binary_index_html_start");
extern const char kIndexHtmlEnd[] asm("_binary_index_html_end");
}

namespace web {
namespace {

constexpr char kTag[] = "web";
constexpr size_t kMaxBody = 48 * 1024;
WebHooks g_hooks;
httpd_handle_t g_server = nullptr;

esp_err_t SendJson(httpd_req_t* req, const std::string& json, const char* status = "200 OK") {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.data(), static_cast<ssize_t>(json.size()));
}

esp_err_t SendError(httpd_req_t* req, const std::string& msg) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg.c_str());
    char* t = cJSON_PrintUnformatted(o);
    const std::string s = t ? t : "{}";
    cJSON_free(t);
    cJSON_Delete(o);
    return SendJson(req, s, "400 Bad Request");
}

bool ReadBody(httpd_req_t* req, std::string* out) {
    if (req->content_len > kMaxBody) return false;
    out->resize(req->content_len);
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, &(*out)[got], req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// Request comes from a phone on the set-up network (192.168.4.x)?
bool FromSetupNetwork(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
    uint32_t ip = 0;  // host order
    if (addr.ss_family == AF_INET) {
        ip = ntohl(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr.s_addr);
    }
#if LWIP_IPV6
    else if (addr.ss_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<sockaddr_in6*>(&addr);
        const uint8_t* b = a6->sin6_addr.s6_addr;
        // IPv4-mapped ::ffff:a.b.c.d
        static const uint8_t kPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (std::memcmp(b, kPrefix, 12) != 0) return false;
        ip = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) | (uint32_t(b[14]) << 8) | b[15];
    }
#endif
    return net::GetStatus().ap_active && (ip & 0xFFFFFF00u) == 0xC0A80400u;  // 192.168.4.0/24
}

bool Authorized(httpd_req_t* req) {
    if (g_hooks.access_code.empty() || FromSetupNetwork(req)) return true;
    char key[16] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Key", key, sizeof(key)) == ESP_OK &&
        strcasecmp(key, g_hooks.access_code.c_str()) == 0) {
        return true;
    }
    char query[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "k", key, sizeof(key)) == ESP_OK &&
        strcasecmp(key, g_hooks.access_code.c_str()) == 0) {
        return true;
    }
    return false;
}

esp_err_t Denied(httpd_req_t* req) {
    return SendJson(req, "{\"ok\":false,\"error\":\"need_code\"}", "401 Unauthorized");
}

#define REQUIRE_KEY(req) \
    do {                     \
        if (!Authorized(req)) return Denied(req); \
    } while (0)

esp_err_t Index(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // EMBED_TXTFILES appends a NUL terminator.
    return httpd_resp_send(req, kIndexHtmlStart, kIndexHtmlEnd - kIndexHtmlStart - 1);
}

esp_err_t State(httpd_req_t* req) {
    REQUIRE_KEY(req);
    return SendJson(req, g_hooks.state_json());
}

esp_err_t SaveConfig(httpd_req_t* req) {
    REQUIRE_KEY(req);
    std::string body, err;
    if (!ReadBody(req, &body)) return SendError(req, "資料太大或傳送失敗");
    if (!g_hooks.save_config(body, &err)) return SendError(req, err.empty() ? "儲存失敗" : err);
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t Refresh(httpd_req_t* req) {
    REQUIRE_KEY(req);
    g_hooks.refresh_now();
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t Scan(httpd_req_t* req) {
    REQUIRE_KEY(req);
    const auto list = net::Scan();
    cJSON* arr = cJSON_CreateArray();
    for (const auto& e : list) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", e.ssid.c_str());
        cJSON_AddNumberToObject(o, "rssi", e.rssi);
        cJSON_AddBoolToObject(o, "secure", e.secure);
        cJSON_AddItemToArray(arr, o);
    }
    char* t = cJSON_PrintUnformatted(arr);
    const std::string s = t ? t : "[]";
    cJSON_free(t);
    cJSON_Delete(arr);
    return SendJson(req, s);
}

esp_err_t SaveWifi(httpd_req_t* req) {
    REQUIRE_KEY(req);
    std::string body;
    if (!ReadBody(req, &body)) return SendError(req, "傳送失敗");
    cJSON* o = cJSON_Parse(body.c_str());
    const cJSON* ssid = cJSON_GetObjectItemCaseSensitive(o, "ssid");
    const cJSON* pass = cJSON_GetObjectItemCaseSensitive(o, "pass");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' ||
        std::strlen(ssid->valuestring) > 32 ||
        (cJSON_IsString(pass) && std::strlen(pass->valuestring) > 63)) {
        cJSON_Delete(o);
        return SendError(req, "Wi-Fi 名稱或密碼不正確");
    }
    const std::string s = ssid->valuestring;
    const std::string p = cJSON_IsString(pass) ? pass->valuestring : "";
    cJSON_Delete(o);
    SendJson(req, "{\"ok\":true}");
    g_hooks.save_wifi(s, p);
    return ESP_OK;
}

esp_err_t UpdateCheck(httpd_req_t* req) {
    REQUIRE_KEY(req);
    return SendJson(req, g_hooks.update_check());
}

esp_err_t UpdateInstall(httpd_req_t* req) {
    REQUIRE_KEY(req);
    std::string err;
    if (!g_hooks.update_install(&err)) return SendError(req, err);
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t Screen(httpd_req_t* req) {
    REQUIRE_KEY(req);
    const std::string bmp = g_hooks.screen_bmp();
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, bmp.data(), static_cast<ssize_t>(bmp.size()));
}

// Anything unknown: in set-up mode send the phone to the settings page so the
// "sign in to network" pop-up appears; otherwise a plain 404.
esp_err_t NotFound(httpd_req_t* req, httpd_err_code_t) {
    if (net::GetStatus().ap_active) {
        httpd_resp_set_status(req, "302 Found");
        const std::string loc = std::string("http://") + net::kSetupIp + "/";
        httpd_resp_set_hdr(req, "Location", loc.c_str());
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "", 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

}  // namespace

bool Start(const WebHooks& hooks) {
    if (g_server != nullptr) return true;
    g_hooks = hooks;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 14;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 6;
    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed");
        g_server = nullptr;
        return false;
    }
    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, Index, nullptr},
        {"/api/state", HTTP_GET, State, nullptr},
        {"/api/config", HTTP_POST, SaveConfig, nullptr},
        {"/api/refresh", HTTP_POST, Refresh, nullptr},
        {"/api/scan", HTTP_GET, Scan, nullptr},
        {"/api/wifi", HTTP_POST, SaveWifi, nullptr},
        {"/screen.bmp", HTTP_GET, Screen, nullptr},
        {"/api/update/check", HTTP_GET, UpdateCheck, nullptr},
        {"/api/update/install", HTTP_POST, UpdateInstall, nullptr},
    };
    for (const auto& u : uris) httpd_register_uri_handler(g_server, &u);
    httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND, NotFound);
    ESP_LOGI(kTag, "web server started");
    return true;
}

}  // namespace web
