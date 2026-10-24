#include "net.h"

#include <sys/time.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>

#include "dns_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs.h"

namespace net {
namespace {

constexpr char kTag[] = "net";
constexpr char kNvsNs[] = "hkbus";
constexpr EventBits_t kConnectedBit = BIT0;

EventGroupHandle_t g_events = nullptr;
esp_netif_t* g_sta = nullptr;
esp_netif_t* g_ap = nullptr;
esp_timer_handle_t g_retry_timer = nullptr;
std::mutex g_mutex;
Status g_status;
bool g_have_creds = false;
bool g_sntp_started = false;
bool g_sntp_synced = false;
int g_retry_delay_ms = 1000;

void RetryConnect(void*) {
    if (g_have_creds) esp_wifi_connect();
}

void OnWifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_have_creds) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
        ESP_LOGW(kTag, "disconnected, reason %d; retry in %d ms", ev->reason, g_retry_delay_ms);
        xEventGroupClearBits(g_events, kConnectedBit);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status.sta_connected = false;
            g_status.ip.clear();
        }
        if (g_have_creds) {
            esp_timer_stop(g_retry_timer);
            esp_timer_start_once(g_retry_timer, static_cast<uint64_t>(g_retry_delay_ms) * 1000);
            // Back off up to 30 s so a wrong password does not keep the radio
            // busy while someone is using the set-up network.
            g_retry_delay_ms = std::min(g_retry_delay_ms * 2, 30000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* ev = static_cast<ip_event_got_ip_t*>(data);
        char ip[16];
        esp_ip4addr_ntoa(&ev->ip_info.ip, ip, sizeof(ip));
        ESP_LOGI(kTag, "got ip %s", ip);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status.sta_connected = true;
            g_status.ip = ip;
        }
        g_retry_delay_ms = 1000;
        xEventGroupSetBits(g_events, kConnectedBit);
        StartTimeSync();
    }
}

bool TimeLooksValid() { return time(nullptr) > 1700000000; }

void OnSntpSync(struct timeval*) {
    g_sntp_synced = true;
    ESP_LOGI(kTag, "clock synchronised by SNTP");
}

}  // namespace

esp_err_t Init() {
    setenv("TZ", "HKT-8", 1);
    tzset();
    g_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    g_sta = esp_netif_create_default_wifi_sta();
    g_ap = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, OnWifiEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, OnWifiEvent, nullptr);
    esp_timer_create_args_t targs = {};
    targs.callback = RetryConnect;
    targs.name = "wifi_retry";
    esp_timer_create(&targs, &g_retry_timer);

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char name[32];
    snprintf(name, sizeof(name), "HKBus-%02X%02X", mac[4], mac[5]);
    g_status.ap_ssid = name;

    // Set-up network password: 8 random digits, kept in NVS so the code on
    // the screen stays the same across restarts.
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
        char pass[16] = {};
        size_t len = sizeof(pass);
        if (nvs_get_str(h, "ap_pass", pass, &len) != ESP_OK || std::strlen(pass) != 8) {
            for (int i = 0; i < 8; ++i) pass[i] = static_cast<char>('0' + esp_random() % 10);
            pass[8] = '\0';
            nvs_set_str(h, "ap_pass", pass);
            nvs_commit(h);
        }
        g_status.ap_pass = pass;
        nvs_close(h);
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    return esp_wifi_start();
}

bool LoadCredentials(std::string* ssid, std::string* pass) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return false;
    char s[33] = {}, p[65] = {};
    size_t sl = sizeof(s), pl = sizeof(p);
    const bool ok = nvs_get_str(h, "wifi_ssid", s, &sl) == ESP_OK && s[0] != '\0';
    if (ok && nvs_get_str(h, "wifi_pass", p, &pl) != ESP_OK) p[0] = '\0';
    nvs_close(h);
    if (ok) {
        *ssid = s;
        *pass = p;
    }
    return ok;
}

esp_err_t SaveCredentials(const std::string& ssid, const std::string& pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "wifi_ssid", ssid.c_str());
    if (err == ESP_OK) err = nvs_set_str(h, "wifi_pass", pass.c_str());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void StartStation(const std::string& ssid, const std::string& pass) {
    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), ssid.c_str(), sizeof(wc.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wc.sta.password), pass.c_str(), sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.sta_ssid = ssid;
    }
    g_have_creds = true;
    g_retry_delay_ms = 1000;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
}

void StartSetupAp() {
    if (g_status.ap_active) return;
    wifi_config_t ap = {};
    std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), g_status.ap_ssid.c_str(), sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = static_cast<uint8_t>(g_status.ap_ssid.size());
    std::strncpy(reinterpret_cast<char*>(ap.ap.password), g_status.ap_pass.c_str(),
                 sizeof(ap.ap.password) - 1);
    ap.ap.authmode = g_status.ap_pass.size() >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    dns_server::Start();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.ap_active = true;
    ESP_LOGI(kTag, "set-up network %s started", g_status.ap_ssid.c_str());
}

void StopSetupAp() {
    if (!g_status.ap_active) return;
    dns_server::Stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.ap_active = false;
    ESP_LOGI(kTag, "set-up network stopped");
}

bool WaitConnected(TickType_t timeout) {
    return (xEventGroupWaitBits(g_events, kConnectedBit, pdFALSE, pdTRUE, timeout) &
            kConnectedBit) != 0;
}

Status GetStatus() {
    Status s;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        s = g_status;
    }
    if (s.sta_connected) {
        wifi_ap_record_t info = {};
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) s.rssi = info.rssi;
    }
    s.time_valid = TimeValid();
    return s;
}

std::vector<ScanEntry> Scan() {
    std::vector<ScanEntry> out;
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) return out;
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return out;
    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) return out;
    for (uint16_t i = 0; i < n; ++i) {
        const std::string ssid(reinterpret_cast<const char*>(recs[i].ssid));
        if (ssid.empty()) continue;
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const ScanEntry& e) { return e.ssid == ssid; });
        if (it != out.end()) {
            it->rssi = std::max(it->rssi, static_cast<int>(recs[i].rssi));
            continue;
        }
        out.push_back({ssid, recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN});
    }
    std::sort(out.begin(), out.end(), [](const ScanEntry& a, const ScanEntry& b) {
        return a.rssi > b.rssi;
    });
    return out;
}

void StartTimeSync() {
    if (g_sntp_started) return;
    g_sntp_started = true;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("stdtime.gov.hk", "pool.ntp.org", "time.google.com"));
    config.sync_cb = OnSntpSync;
    esp_netif_sntp_init(&config);
}

bool TimeValid() { return TimeLooksValid(); }

void SetTimeFromHttpDate(const char* date) {
    // Only used until SNTP has answered (e.g. NTP blocked by the router).
    if (g_sntp_synced || date == nullptr) return;
    // "Sat, 24 Oct 2026 09:13:05 GMT"
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int d, y, hh, mm, ss;
    char mon[4] = {};
    if (sscanf(date, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return;
    int m = -1;
    for (int i = 0; i < 12; ++i) {
        if (std::strcmp(mon, kMonths[i]) == 0) m = i;
    }
    if (m < 0) return;
    // days from civil (UTC)
    int yy = y - (m < 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned mp = static_cast<unsigned>((m + 1) + ((m + 1) > 2 ? -3 : 9));
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    const int64_t epoch = days * 86400 + hh * 3600 + mm * 60 + ss;
    const int64_t now = time(nullptr);
    if (now < 1700000000 || std::llabs(now - epoch) > 90) {
        timeval tv = {static_cast<time_t>(epoch), 0};
        settimeofday(&tv, nullptr);
        ESP_LOGI(kTag, "clock set from HTTP Date header");
    }
}

}  // namespace net
