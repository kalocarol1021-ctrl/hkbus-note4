#include "ota.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "http_fetch.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

namespace ota {
namespace {

constexpr char kTag[] = "ota";

std::string JsonStr(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

std::string Hex(const uint8_t* d, size_t n) {
    std::string out;
    char b[3];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02x", d[i]);
        out += b;
    }
    return out;
}

}  // namespace

int CompareVersions(const std::string& a, const std::string& b) {
    const char* pa = a.c_str();
    const char* pb = b.c_str();
    if (*pa == 'v') ++pa;
    if (*pb == 'v') ++pb;
    for (int i = 0; i < 4; ++i) {
        char* ea = nullptr;
        char* eb = nullptr;
        const long va = std::strtol(pa, &ea, 10);
        const long vb = std::strtol(pb, &eb, 10);
        if (va != vb) return va < vb ? -1 : 1;
        pa = (*ea == '.') ? ea + 1 : ea;
        pb = (*eb == '.') ? eb + 1 : eb;
        if (*pa == '\0' && *pb == '\0') break;
    }
    return 0;
}

Manifest Check() {
    Manifest m;
    HttpFetcher fetcher;
    std::string body;
    if (!fetcher.Fetch(CONFIG_HKBUS_UPDATE_URL, "", &body)) {
        m.error = "未能連接更新伺服器";
        return m;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        m.error = "更新資料格式錯誤";
        return m;
    }
    m.version = JsonStr(root, "version");
    m.url = JsonStr(root, "url");
    m.sha256 = JsonStr(root, "sha256");
    m.notes = JsonStr(root, "notes");
    const cJSON* size = cJSON_GetObjectItemCaseSensitive(root, "size");
    m.size = cJSON_IsNumber(size) ? static_cast<int>(size->valuedouble) : 0;
    cJSON_Delete(root);
    if (m.version.empty() || m.url.rfind("https://", 0) != 0 || m.sha256.size() != 64) {
        m.error = "更新資料不完整";
        return m;
    }
    for (char& c : m.sha256) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    m.ok = true;
    return m;
}

esp_err_t Install(const Manifest& m, const std::function<void(int)>& progress,
                  std::string* error) {
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        *error = "沒有可用的更新分區";
        return ESP_ERR_NOT_FOUND;
    }
    if (m.size > 0 && static_cast<size_t>(m.size) > target->size) {
        *error = "新固件太大";
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = m.url.c_str();
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.max_redirection_count = 5;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        *error = "未能建立連線";
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    int64_t total = 0;
    if (err == ESP_OK) {
        total = esp_http_client_fetch_headers(client);
        // Follow a redirect (e.g. GitHub release assets) manually.
        for (int i = 0; i < 5 && err == ESP_OK; ++i) {
            const int status = esp_http_client_get_status_code(client);
            if (status != 301 && status != 302 && status != 307 && status != 308) break;
            err = esp_http_client_set_redirection(client);
            if (err == ESP_OK) {
                esp_http_client_close(client);
                err = esp_http_client_open(client, 0);
                if (err == ESP_OK) total = esp_http_client_fetch_headers(client);
            }
        }
    }
    if (err != ESP_OK || esp_http_client_get_status_code(client) != 200) {
        ESP_LOGW(kTag, "download failed: %s status %d", esp_err_to_name(err),
                 esp_http_client_get_status_code(client));
        esp_http_client_cleanup(client);
        *error = "下載失敗";
        return err != ESP_OK ? err : ESP_FAIL;
    }
    if (total <= 0) total = m.size;

    esp_ota_handle_t handle = 0;
    err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        *error = "未能開始寫入";
        return err;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    std::unique_ptr<char[]> buf(new char[4096]);
    int64_t got = 0;
    int last_pct = -1;
    while (true) {
        const int n = esp_http_client_read(client, buf.get(), 4096);
        if (n < 0) {
            err = ESP_FAIL;
            *error = "下載中斷";
            break;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            if (total > 0 && got >= total) break;
            err = ESP_FAIL;
            *error = "下載中斷";
            break;
        }
        mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char*>(buf.get()), n);
        err = esp_ota_write(handle, buf.get(), n);
        if (err != ESP_OK) {
            *error = "寫入失敗";
            break;
        }
        got += n;
        if (total > 0) {
            const int pct = static_cast<int>(got * 100 / total);
            if (pct != last_pct) {
                last_pct = pct;
                progress(pct);
            }
        }
    }
    esp_http_client_cleanup(client);
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (err == ESP_OK && Hex(digest, sizeof(digest)) != m.sha256) {
        ESP_LOGW(kTag, "sha256 mismatch");
        *error = "檔案校驗失敗";
        err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        return err;
    }
    err = esp_ota_end(handle);  // also validates the image structure
    if (err != ESP_OK) {
        *error = "固件檢查失敗";
        return err;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) *error = "未能切換到新固件";
    return err;
}

void MarkRunningValid() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(kTag, "new firmware confirmed");
    }
}

}  // namespace ota
