// ZECTRIX NOTE4 Hong Kong bus / minibus arrival board.
//
// Screens are drawn by components/hkbus_core (shared with the PC preview);
// this file wires them to the board: e-paper refresh policy, buttons, Wi-Fi,
// the settings web page and the once-a-minute arrival fetch.
#include <sys/time.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hk_fb.h"
#include "hk_font.h"
#include "hk_model.h"
#include "hk_screen.h"
#include "hk_weather.h"
#include "http_fetch.h"
#include "net.h"
#include "ota.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rtc_pcf8563.h"
#include "web_server.h"
#include "zectrix_board.h"
#include "zectrix_epd.h"

#define EMBED(name)                                                   \
    extern const uint8_t name##_start[] asm("_binary_" #name "_bin_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_bin_end");
extern "C" {
EMBED(cjk20)
EMBED(cjk26)
EMBED(cjk32)
EMBED(num44)
EMBED(num58)
}

namespace {

constexpr char kTag[] = "hkbus";
constexpr char kVersion[] = "1.3.0";
constexpr char kNvsNs[] = "hkbus";
constexpr int64_t kFetchIntervalUs = 60LL * 1000 * 1000;      // once a minute
constexpr int64_t kFullRefreshIntervalUs = 30LL * 60 * 1000 * 1000;
constexpr int kMaxPartialRefreshes = 40;
constexpr int64_t kInfoTimeoutUs = 90LL * 1000 * 1000;

enum class Screen { kNone, kBoard, kSetup, kInfo, kRest, kMessage };

int64_t NowUs() { return esp_timer_get_time(); }

class App {
public:
    void Run();

private:
    // ---- display ----
    void InitFonts();
    void Present(bool force_full);
    void ShowMessage(const std::string& title, const std::vector<std::string>& lines,
                     const std::string& qr = "", const std::string& caption = "");
    void DrawCurrent(bool force_full = false);
    bool ShowNow(const hk::Config& cfg) const;
    std::string SettingsUrl(const net::Status& st) const;
    void LoadOrCreateAccessCode();

    // ---- data ----
    void LoadConfig();
    bool SaveConfig(const std::string& json, std::string* error);
    static void FetchTaskEntry(void* arg) { static_cast<App*>(arg)->FetchTask(); }
    void FetchTask();
    void RequestFetch();
    std::string StateJson();
    std::string ScreenBmp();

    // ---- flow ----
    void HandleButton(const ZectrixButtonEvent& ev);
    void EnterSetup(bool connection_failed);
    void LeaveSetup();
    [[noreturn]] void Shutdown();
    void SyncRtc();

    ZectrixBoard board_;
    zectrix_epd_handle_t epd_ = nullptr;
    hk::Fonts fonts_;
    hk::Framebuffer fb_;      // being drawn
    hk::Framebuffer shown_;   // what is on the panel now
    bool panel_valid_ = false;
    int partial_count_ = 0;
    int64_t last_full_us_ = 0;
    std::mutex display_mu_;

    std::mutex mu_;           // guards cfg_, rows_, generation_
    hk::Config cfg_;
    std::vector<hk::RowState> rows_;
    uint32_t generation_ = 0;
    std::atomic<bool> last_fetch_ok_{true};
    std::atomic<bool> redraw_{false};
    std::atomic<bool> config_changed_{false};
    TaskHandle_t fetch_task_ = nullptr;
    int64_t last_fetch_us_ = -kFetchIntervalUs;

    Screen screen_ = Screen::kNone;
    int page_ = 0;
    int64_t page_shown_us_ = 0;
    int64_t info_shown_us_ = 0;
    bool setup_connection_failed_ = false;
    int last_drawn_minute_ = -1;
    int battery_percent_ = -1;
    int64_t last_battery_us_ = -kFetchIntervalUs;
    std::atomic<int64_t> restart_at_us_{0};
    bool rtc_written_ = false;
    std::string pending_ssid_;
    std::string access_code_;               // 6-character settings code
    std::atomic<int64_t> awake_until_us_{0};  // Home key outside display hours

    // ---- firmware update ----
    std::string UpdateCheckJson();
    bool StartUpdate(std::string* error);
    static void UpdateTaskEntry(void* arg) { static_cast<App*>(arg)->UpdateTask(); }
    void UpdateTask();
    std::mutex update_mu_;
    ota::Manifest manifest_;           // last successful check
    std::atomic<int> update_state_{0};  // 0 idle, 1 running, 2 failed, 3 done
    std::atomic<int> update_progress_{0};
    std::string update_error_;
    int update_drawn_bucket_ = -1;
    bool marked_valid_ = false;

    // ---- Hong Kong Observatory (guarded by mu_) ----
    void FetchWeather(HttpFetcher& fetcher, const hk::Config& cfg);
    hk::WeatherView WeatherForScreen(const hk::Config& cfg);
    hk::WeatherNow weather_now_;
    std::vector<hk::Warning> warnings_;
    int64_t weather_at_ = 0;      // last successful fetch (epoch)
    int64_t warnings_at_ = 0;
    int64_t weather_try_ = 0;     // last attempt (epoch)
    int64_t warnings_try_ = 0;
};

constexpr int64_t kTemporaryAwakeUs = 10LL * 60 * 1000 * 1000;

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void App::InitFonts() {
    fonts_.s20.Load(cjk20_start, cjk20_end - cjk20_start);
    fonts_.m26.Load(cjk26_start, cjk26_end - cjk26_start);
    fonts_.l32.Load(cjk32_start, cjk32_end - cjk32_start);
    fonts_.n44.Load(num44_start, num44_end - num44_start);
    fonts_.n58.Load(num58_start, num58_end - num58_start);
    // Route numbers / minutes fonts only carry Latin glyphs.
    fonts_.n44.SetFallback(&fonts_.l32);
    fonts_.n58.SetFallback(&fonts_.l32);
}

// Push fb_ to the panel. Uses a partial refresh of just the rows that changed
// (no black flash), with a periodic full refresh to clear ghosting.
void App::Present(bool force_full) {
    std::lock_guard<std::mutex> lock(display_mu_);
    if (epd_ == nullptr) return;
    const int64_t now = NowUs();
    const bool full = force_full || !panel_valid_ || partial_count_ >= kMaxPartialRefreshes ||
                      now - last_full_us_ >= kFullRefreshIntervalUs;
    int y0 = -1, y1 = -1;
    if (!full) {
        constexpr int stride = hk::Framebuffer::kStride;
        for (int y = 0; y < hk::Framebuffer::kHeight; ++y) {
            if (std::memcmp(fb_.data() + y * stride, shown_.data() + y * stride, stride) != 0) {
                if (y0 < 0) y0 = y;
                y1 = y;
            }
        }
        if (y0 < 0) return;  // nothing changed
    }

    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err == ESP_OK) {
        if (full) {
            err = zectrix_epd_refresh_full_1bpp(epd_, fb_.data(), fb_.size());
        } else {
            const zectrix_epd_rect_t rect = {0, y0, hk::Framebuffer::kWidth, y1 - y0 + 1};
            err = zectrix_epd_refresh_partial_1bpp(
                epd_, &rect, fb_.data() + y0 * hk::Framebuffer::kStride,
                static_cast<size_t>(rect.height) * hk::Framebuffer::kStride);
        }
    }
    zectrix_epd_power_off(epd_);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "e-paper refresh failed: %s", esp_err_to_name(err));
        panel_valid_ = false;  // next time do a full refresh
        return;
    }
    std::memcpy(shown_.data(), fb_.data(), fb_.size());
    panel_valid_ = true;
    if (full) {
        partial_count_ = 0;
        last_full_us_ = now;
    } else {
        ++partial_count_;
    }
}

void App::ShowMessage(const std::string& title, const std::vector<std::string>& lines,
                      const std::string& qr, const std::string& caption) {
    hk::DrawMessage(fb_, fonts_, title, lines, qr, caption);
    Present(true);
}

// Outside the configured display hours the board rests, unless someone
// pressed Home to look at it for a while.
bool App::ShowNow(const hk::Config& cfg) const {
    if (NowUs() < awake_until_us_) return true;
    if (!cfg.hours_on || !net::TimeValid()) return true;
    int hh, mm;
    hk::HkClock(time(nullptr), &hh, &mm);
    return hk::IsShowTime(cfg, hh * 60 + mm);
}

std::string App::SettingsUrl(const net::Status& st) const {
    if (!st.sta_connected) return "";
    return "http://" + st.ip + "/?k=" + access_code_;
}

// The settings page and API need this code (shown on the device), so other
// people on the same network cannot change the board.
void App::LoadOrCreateAccessCode() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    char buf[16] = {};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, "access_code", buf, &len) == ESP_OK && std::strlen(buf) == 6) {
        access_code_ = buf;
    } else {
        // No 0/O/1/I/L to avoid misreading on the screen.
        static const char kAlphabet[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
        access_code_.clear();
        for (int i = 0; i < 6; ++i) {
            access_code_ += kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
        }
        nvs_set_str(h, "access_code", access_code_.c_str());
        nvs_commit(h);
    }
    nvs_close(h);
}

// Draw whatever the current screen is and push it if it changed.
void App::DrawCurrent(bool force_full) {
    const net::Status st = net::GetStatus();
    const Screen before = screen_;

    if (screen_ == Screen::kSetup) {
        std::vector<std::string> lines;
        if (setup_connection_failed_ && !st.sta_ssid.empty()) {
            lines.push_back("未能連接「" + st.sta_ssid + "」");
        }
        lines.push_back("掃描右邊 QR Code");
        lines.push_back("或手機連接 Wi-Fi：");
        lines.push_back(st.ap_ssid);
        lines.push_back("密碼：" + st.ap_pass);
        lines.push_back("再開啟 " + std::string(net::kSetupIp));
        hk::DrawMessage(fb_, fonts_, "設定 Wi-Fi", lines,
                        "WIFI:T:WPA;S:" + st.ap_ssid + ";P:" + st.ap_pass + ";;", "掃描連接");
    } else if (screen_ == Screen::kInfo) {
        std::vector<std::string> lines;
        lines.push_back(st.sta_connected ? "Wi-Fi：" + st.sta_ssid : "Wi-Fi：未連接");
        if (st.sta_connected) lines.push_back("網址：" + st.ip);
        lines.push_back("設定碼：" + access_code_);
        if (battery_percent_ >= 0) lines.push_back("電量：" + std::to_string(battery_percent_) + "%");
        lines.push_back("音量＋：重設 Wi-Fi");
        lines.push_back("Home 鍵：返回");
        const std::string url = SettingsUrl(st);
        hk::DrawMessage(fb_, fonts_, std::string("設定資料 v") + kVersion, lines, url,
                        url.empty() ? "" : "掃描開啟設定頁");
    } else {
        hk::Config cfg;
        hk::BoardView view;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cfg = cfg_;
            view.rows = rows_;
        }
        if (cfg.items.empty()) {
            screen_ = Screen::kMessage;
            std::vector<std::string> lines = {"用連接同一 Wi-Fi 的", "手機掃描右邊 QR Code，",
                                              "或開啟以下網址："};
            lines.push_back(st.sta_connected ? st.ip : "（連接 Wi-Fi 中…）");
            lines.push_back("設定碼：" + access_code_);
            hk::DrawMessage(fb_, fonts_, "未設定車站", lines, SettingsUrl(st), "掃描開啟設定頁");
        } else if (!ShowNow(cfg)) {
            screen_ = Screen::kRest;
            int hh, mm;
            hk::HkClock(time(nullptr), &hh, &mm);
            const int next = hk::NextShowStart(cfg, hh * 60 + mm);
            std::vector<std::string> lines = {"休息中，暫停更新"};
            if (next >= 0) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "%02d:%02d 恢復顯示", next / 60, next % 60);
                lines.push_back(buf);
            }
            lines.push_back("");
            lines.push_back("按 Home 鍵臨時顯示 10 分鐘");
            hk::DrawMessage(fb_, fonts_, cfg.title.empty() ? "巴士到站時間" : cfg.title, lines);
        } else {
            screen_ = Screen::kBoard;
            const int pages = hk::PageCount(cfg);
            if (page_ >= pages) page_ = 0;
            view.config = &cfg;
            view.page = page_;
            view.now = net::TimeValid() ? time(nullptr) : 0;
            view.network_ok = st.sta_connected && last_fetch_ok_;
            view.battery_percent = battery_percent_;
            view.weather = WeatherForScreen(cfg);
            hk::DrawBoard(fb_, fonts_, view);
        }
    }
    // A change of screen type gets a clean full refresh.
    if (before != screen_) force_full = true;
    Present(force_full);
}

// ---------------------------------------------------------------------------
// Configuration and data
// ---------------------------------------------------------------------------

void App::LoadConfig() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, "config", nullptr, &len) == ESP_OK && len > 0) {
        std::string json(len, '\0');
        if (nvs_get_blob(h, "config", &json[0], &len) == ESP_OK) {
            std::string err;
            hk::Config cfg;
            if (hk::ParseConfig(json.c_str(), &cfg, &err)) {
                std::lock_guard<std::mutex> lock(mu_);
                cfg_ = cfg;
                rows_.assign(cfg_.items.size(), {});
            }
        }
    }
    nvs_close(h);
}

bool App::SaveConfig(const std::string& json, std::string* error) {
    hk::Config cfg;
    if (!hk::ParseConfig(json.c_str(), &cfg, error)) return false;
    const std::string normalized = hk::SerializeConfig(cfg);
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "config", normalized.data(), normalized.size());
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        *error = std::string("儲存失敗：") + esp_err_to_name(err);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = cfg;
        rows_.assign(cfg_.items.size(), {});
        ++generation_;
        weather_try_ = 0;
        weather_now_ = hk::WeatherNow();
    }
    config_changed_ = true;
    RequestFetch();
    return true;
}

// Temperature/weather every 10 minutes (the Observatory updates hourly),
// warnings every 5 minutes.
void App::FetchWeather(HttpFetcher& fetcher, const hk::Config& cfg) {
    const int64_t now = time(nullptr);
    std::string body;
    if (now - weather_try_ >= 600) {
        weather_try_ = now;
        hk::WeatherNow w;
        if (fetcher.Fetch(hk::kHkoCurrentUrl, "", &body) &&
            hk::ParseHkoCurrent(body.c_str(), cfg.weather_place, &w)) {
            std::lock_guard<std::mutex> lock(mu_);
            weather_now_ = w;
            weather_at_ = now;
        } else {
            ESP_LOGW(kTag, "HKO weather fetch failed");
        }
    }
    if (now - warnings_try_ >= 300) {
        warnings_try_ = now;
        std::vector<hk::Warning> ws;
        if (fetcher.Fetch(hk::kHkoWarningsUrl, "", &body) && hk::ParseHkoWarnings(body.c_str(), &ws)) {
            std::lock_guard<std::mutex> lock(mu_);
            warnings_ = ws;
            warnings_at_ = now;
        } else {
            ESP_LOGW(kTag, "HKO warnings fetch failed");
        }
    }
}

hk::WeatherView App::WeatherForScreen(const hk::Config& cfg) {
    hk::WeatherView v;
    v.show = cfg.weather_on;
    if (!v.show) return v;
    const int64_t now = time(nullptr);
    std::lock_guard<std::mutex> lock(mu_);
    // Old data is hidden rather than shown as if current; warnings in
    // particular must not linger after they were cancelled.
    if (weather_now_.ok && now - weather_at_ < 90 * 60) {
        v.has_now = true;
        v.place = weather_now_.place;
        v.temperature = weather_now_.temperature;
        v.desc = weather_now_.desc;
    }
    if (now - warnings_at_ < 30 * 60) v.warnings = warnings_;
    return v;
}

void App::RequestFetch() {
    if (fetch_task_ != nullptr) xTaskNotifyGive(fetch_task_);
}

void App::FetchTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        hk::Config cfg;
        uint32_t gen;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cfg = cfg_;
            gen = generation_;
        }
        if (cfg.items.empty() || !ShowNow(cfg) || update_state_ == 1) continue;
        if (!net::GetStatus().sta_connected) {
            last_fetch_ok_ = false;
            std::lock_guard<std::mutex> lock(mu_);
            if (gen == generation_) {
                for (auto& r : rows_) r.failed = true;
            }
            redraw_ = true;
            continue;
        }

        HttpFetcher fetcher;
        bool any_ok = false;
        std::string body;
        for (size_t i = 0; i < cfg.items.size(); ++i) {
            const hk::Item& item = cfg.items[i];
            std::vector<hk::Eta> etas;
            bool item_ok = false;
            for (const hk::HttpRequest& req : hk::BuildRequests(item)) {
                if (!fetcher.Fetch(req.url, req.post_body, &body)) continue;
                if (hk::ParseEtaResponse(item, item.cos[req.co_index], body.c_str(),
                                         time(nullptr), &etas)) {
                    item_ok = true;
                } else {
                    ESP_LOGW(kTag, "unexpected reply for %s", req.url.c_str());
                }
            }
            const int64_t now = time(nullptr);
            hk::MergeEtas(&etas, now);
            any_ok |= item_ok;

            std::lock_guard<std::mutex> lock(mu_);
            if (gen != generation_ || i >= rows_.size()) break;  // config changed meanwhile
            hk::RowState& row = rows_[i];
            row.failed = !item_ok;
            if (item_ok) {
                row.loaded = true;
                row.updated = now;
                row.times.clear();
                row.remark.clear();
                for (const hk::Eta& e : etas) {
                    if (e.time != 0) row.times.push_back(e.time);
                    else if (row.remark.empty()) row.remark = e.remark;
                }
            }
        }
        if (cfg.weather_on) FetchWeather(fetcher, cfg);
        fetcher.CloseAll();
        last_fetch_ok_ = any_ok;
        ESP_LOGI(kTag, "fetch done (%s), free heap %u", any_ok ? "ok" : "failed",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        redraw_ = true;
    }
}

std::string App::StateJson() {
    const net::Status st = net::GetStatus();
    hk::Config cfg;
    std::vector<hk::RowState> rows;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg = cfg_;
        rows = rows_;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", kVersion);
    cJSON_AddStringToObject(root, "mode", st.ap_active ? "setup" : "normal");
    cJSON_AddBoolToObject(root, "connected", st.sta_connected);
    cJSON_AddStringToObject(root, "ssid", st.sta_ssid.c_str());
    cJSON_AddStringToObject(root, "ip", st.ip.c_str());
    cJSON_AddNumberToObject(root, "rssi", st.rssi);
    cJSON_AddStringToObject(root, "ap_ssid", st.ap_ssid.c_str());
    cJSON_AddNumberToObject(root, "time", st.time_valid ? static_cast<double>(time(nullptr)) : 0);
    cJSON_AddNumberToObject(root, "battery", battery_percent_);
    {
        const hk::WeatherView wv = WeatherForScreen(cfg);
        cJSON* w = cJSON_AddObjectToObject(root, "weather");
        cJSON_AddBoolToObject(w, "ok", wv.has_now);
        cJSON_AddStringToObject(w, "place", wv.place.c_str());
        cJSON_AddNumberToObject(w, "temp", wv.temperature);
        cJSON_AddStringToObject(w, "desc", wv.desc.c_str());
        cJSON* arr = cJSON_AddArrayToObject(w, "warnings");
        for (const hk::Warning& x : wv.warnings) cJSON_AddItemToArray(arr, cJSON_CreateString(x.text.c_str()));
    }
    {
        static const char* kStates[] = {"idle", "running", "failed", "done"};
        cJSON* up = cJSON_AddObjectToObject(root, "update");
        cJSON_AddStringToObject(up, "state", kStates[update_state_ & 3]);
        cJSON_AddNumberToObject(up, "progress", update_progress_);
        std::lock_guard<std::mutex> lock(update_mu_);
        cJSON_AddStringToObject(up, "error", update_error_.c_str());
    }
    cJSON_AddItemToObject(root, "config", cJSON_Parse(hk::SerializeConfig(cfg).c_str()));
    cJSON* arr = cJSON_AddArrayToObject(root, "rows");
    const int64_t now = time(nullptr);
    for (const hk::RowState& r : rows) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "loaded", r.loaded);
        cJSON_AddBoolToObject(o, "failed", r.failed);
        cJSON_AddStringToObject(o, "remark", r.remark.c_str());
        const bool stale = r.updated > 0 && now - r.updated > hk::kStaleAfterSeconds;
        cJSON_AddBoolToObject(o, "stale", stale);
        cJSON* mins = cJSON_AddArrayToObject(o, "mins");
        for (int64_t t : r.times) {
            if (stale) break;
            const int m = hk::MinutesUntil(t, now);
            if (m >= 0) cJSON_AddItemToArray(mins, cJSON_CreateNumber(m));
        }
        cJSON_AddItemToArray(arr, o);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

std::string App::ScreenBmp() {
    constexpr int W = hk::Framebuffer::kWidth, H = hk::Framebuffer::kHeight;
    constexpr int row = ((W + 31) / 32) * 4;  // 52 bytes
    constexpr int header = 14 + 40 + 8;
    std::string bmp(header + row * H, '\0');
    auto put32 = [&](int off, uint32_t v) {
        for (int i = 0; i < 4; ++i) bmp[off + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    auto put16 = [&](int off, uint16_t v) {
        bmp[off] = static_cast<char>(v & 0xFF);
        bmp[off + 1] = static_cast<char>(v >> 8);
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, static_cast<uint32_t>(bmp.size()));
    put32(10, header);
    put32(14, 40);
    put32(18, W);
    put32(22, H);
    put16(26, 1);
    put16(28, 1);
    put32(34, row * H);
    put32(46, 2);
    // palette: 0 = black, 1 = white (same bit sense as the framebuffer)
    put32(54, 0x00000000);
    put32(58, 0x00FFFFFF);
    std::lock_guard<std::mutex> lock(display_mu_);
    for (int y = 0; y < H; ++y) {
        std::memcpy(&bmp[header + (H - 1 - y) * row], shown_.data() + y * hk::Framebuffer::kStride,
                    hk::Framebuffer::kStride);
    }
    return bmp;
}

// ---------------------------------------------------------------------------
// Flow
// ---------------------------------------------------------------------------

void App::EnterSetup(bool connection_failed) {
    setup_connection_failed_ = connection_failed;
    net::StartSetupAp();
    screen_ = Screen::kSetup;
    DrawCurrent();
}

void App::LeaveSetup() {
    net::StopSetupAp();
    screen_ = Screen::kBoard;
    RequestFetch();
    DrawCurrent();
}

void App::HandleButton(const ZectrixButtonEvent& ev) {
    if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kLongPress) {
        Shutdown();
    }
    switch (screen_) {
        case Screen::kSetup:
            // OK leaves set-up if the home Wi-Fi works again.
            if (ev.button == ZectrixButton::kOk && ev.action == ZectrixButtonAction::kClick &&
                net::GetStatus().sta_connected) {
                LeaveSetup();
            }
            return;
        case Screen::kInfo:
            if (ev.button == ZectrixButton::kUp) {
                EnterSetup(false);
            } else if (ev.button == ZectrixButton::kOk) {
                screen_ = Screen::kBoard;
                DrawCurrent();
            }
            return;
        default:
            break;
    }
    if (ev.button == ZectrixButton::kOk && ev.action == ZectrixButtonAction::kLongPress) {
        screen_ = Screen::kInfo;
        info_shown_us_ = NowUs();
        DrawCurrent();
        return;
    }
    if (ev.button == ZectrixButton::kOk) {
        // Manual refresh: fetch now, and also clean any ghosting. Outside
        // the display hours this shows the board for 10 minutes.
        if (screen_ == Screen::kRest) awake_until_us_ = NowUs() + kTemporaryAwakeUs;
        RequestFetch();
        last_fetch_us_ = NowUs();
        DrawCurrent(true);
        return;
    }
    int pages;
    {
        std::lock_guard<std::mutex> lock(mu_);
        pages = hk::PageCount(cfg_);
    }
    if (pages <= 1) return;
    page_ = ev.button == ZectrixButton::kUp ? (page_ + pages - 1) % pages : (page_ + 1) % pages;
    page_shown_us_ = NowUs();
    DrawCurrent();
}

void App::Shutdown() {
    ESP_LOGI(kTag, "shutting down");
    fb_.Clear();
    Present(true);
    esp_wifi_stop();
    board_.SetPowerLed(false);
    board_.SetAudioPower(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    board_.CutBatteryPower();
    vTaskDelay(pdMS_TO_TICKS(100));
    // Still alive: USB keeps the board powered. Sleep until the power key
    // (DOWN) is pressed again; wait for it to be released first.
    while (gpio_get_level(GPIO_NUM_18) == 0) vTaskDelay(pdMS_TO_TICKS(20));
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_18, 0);
    esp_deep_sleep_start();
}

void App::SyncRtc() {
    RtcPcf8563* rtc = board_.rtc();
    if (rtc == nullptr) return;
    if (!net::TimeValid()) {
        tm t = {};
        if (rtc->GetTime(t) && t.tm_year + 1900 >= 2024) {
            const time_t epoch = mktime(&t);
            timeval tv = {epoch, 0};
            settimeofday(&tv, nullptr);
            ESP_LOGI(kTag, "clock restored from RTC");
        }
        return;
    }
    if (!rtc_written_) {
        const time_t now = time(nullptr);
        tm t = {};
        localtime_r(&now, &t);
        rtc_written_ = rtc->SetTime(t);
    }
}

// ---------------------------------------------------------------------------
// Firmware update
// ---------------------------------------------------------------------------

std::string App::UpdateCheckJson() {
    const ota::Manifest m = ota::Check();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", m.ok);
    cJSON_AddStringToObject(o, "current", kVersion);
    if (m.ok) {
        cJSON_AddStringToObject(o, "latest", m.version.c_str());
        cJSON_AddStringToObject(o, "notes", m.notes.c_str());
        cJSON_AddBoolToObject(o, "available", ota::CompareVersions(m.version, kVersion) > 0);
        std::lock_guard<std::mutex> lock(update_mu_);
        manifest_ = m;
    } else {
        cJSON_AddStringToObject(o, "error", m.error.c_str());
    }
    char* t = cJSON_PrintUnformatted(o);
    std::string out = t ? t : "{}";
    cJSON_free(t);
    cJSON_Delete(o);
    return out;
}

bool App::StartUpdate(std::string* error) {
    if (update_state_ == 1 || update_state_ == 3) {
        *error = "正在更新中";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(update_mu_);
        if (!manifest_.ok || ota::CompareVersions(manifest_.version, kVersion) <= 0) {
            *error = "沒有可用的更新，請先檢查更新";
            return false;
        }
        update_error_.clear();
    }
    update_progress_ = 0;
    update_state_ = 1;
    if (xTaskCreate(UpdateTaskEntry, "ota", 8192, this, 5, nullptr) != pdPASS) {
        update_state_ = 2;
        *error = "記憶體不足";
        return false;
    }
    return true;
}

void App::UpdateTask() {
    ota::Manifest m;
    {
        std::lock_guard<std::mutex> lock(update_mu_);
        m = manifest_;
    }
    std::string err;
    const esp_err_t r = ota::Install(m, [this](int pct) { update_progress_ = pct; }, &err);
    if (r == ESP_OK) {
        update_progress_ = 100;
        update_state_ = 3;
    } else {
        ESP_LOGW(kTag, "update failed: %s (%s)", esp_err_to_name(r), err.c_str());
        {
            std::lock_guard<std::mutex> lock(update_mu_);
            update_error_ = err.empty() ? "未知錯誤" : err;
        }
        update_drawn_bucket_ = 0;  // tells the main loop to show the failure
        update_state_ = 2;
    }
    vTaskDelete(nullptr);
}

void App::Run() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    if (board_.Init() != ESP_OK) ESP_LOGE(kTag, "board init failed");
    board_.SetAudioPower(false);  // no sound needed

    zectrix_epd_config_t epd_cfg = {};
    zectrix_epd_get_default_config(&epd_cfg);
    if (zectrix_epd_new(&epd_cfg, &epd_) != ESP_OK) {
        ESP_LOGE(kTag, "EPD init failed");
        epd_ = nullptr;
    }
    InitFonts();
    setenv("TZ", "HKT-8", 1);
    tzset();
    SyncRtc();
    LoadConfig();
    LoadOrCreateAccessCode();
    net::Init();

    WebHooks hooks;
    hooks.state_json = [this] { return StateJson(); };
    hooks.save_config = [this](const std::string& j, std::string* e) { return SaveConfig(j, e); };
    hooks.refresh_now = [this] { RequestFetch(); };
    hooks.screen_bmp = [this] { return ScreenBmp(); };
    hooks.update_check = [this] { return UpdateCheckJson(); };
    hooks.update_install = [this](std::string* e) { return StartUpdate(e); };
    hooks.save_wifi = [this](const std::string& ssid, const std::string& pass) {
        net::SaveCredentials(ssid, pass);
        pending_ssid_ = ssid;
        restart_at_us_ = NowUs() + 1500 * 1000;
    };
    hooks.access_code = access_code_;
    web::Start(hooks);
    xTaskCreatePinnedToCore(FetchTaskEntry, "fetch", 12288, this, 4, &fetch_task_, 1);

    std::string ssid, pass;
    if (net::LoadCredentials(&ssid, &pass)) {
        ShowMessage("巴士到站顯示屏", {"正在連接 Wi-Fi：", ssid});
        net::StartStation(ssid, pass);
        if (net::WaitConnected(pdMS_TO_TICKS(30000))) {
            screen_ = Screen::kBoard;
            DrawCurrent();
        } else {
            EnterSetup(true);
        }
    } else {
        EnterSetup(false);
    }

    int64_t sta_up_since = 0;
    while (true) {
        ZectrixButtonEvent ev;
        if (board_.WaitButton(&ev, pdMS_TO_TICKS(250))) {
            HandleButton(ev);
            continue;
        }
        const int64_t now_us = NowUs();

        // A new firmware that has run for a minute is kept; otherwise the
        // bootloader would return to the previous version on the next reset.
        if (!marked_valid_ && now_us > 60LL * 1000 * 1000) {
            ota::MarkRunningValid();
            marked_valid_ = true;
        }

        // Firmware update in progress: show it, nothing else.
        if (update_state_ == 1 || update_state_ == 3) {
            const int bucket = update_state_ == 3 ? 100 : update_progress_ / 25 * 25;
            if (bucket != update_drawn_bucket_) {
                update_drawn_bucket_ = bucket;
                screen_ = Screen::kMessage;
                ShowMessage("正在更新固件",
                            {update_state_ == 3 ? "更新完成，重新啟動中…"
                                                : "已完成 " + std::to_string(bucket) + "%",
                             "", "請勿拔走電源"});
            }
            if (update_state_ == 3) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
            continue;
        }
        if (update_state_ == 2 && update_drawn_bucket_ >= 0) {
            update_drawn_bucket_ = -1;
            std::string err;
            {
                std::lock_guard<std::mutex> lock(update_mu_);
                err = update_error_;
            }
            ShowMessage("更新失敗", {err, "", "繼續使用目前版本"});
            vTaskDelay(pdMS_TO_TICKS(5000));
            screen_ = Screen::kBoard;
            DrawCurrent(true);
        }

        if (restart_at_us_ != 0 && now_us >= restart_at_us_) {
            ShowMessage("已儲存 Wi-Fi", {"正在重新啟動…", pending_ssid_});
            esp_restart();
        }

        const net::Status st = net::GetStatus();
        SyncRtc();

        // Home Wi-Fi came back while showing the set-up screen: once nobody is
        // connected to the set-up network, return to the board.
        if (screen_ == Screen::kSetup && st.sta_connected) {
            if (sta_up_since == 0) sta_up_since = now_us;
            wifi_sta_list_t list = {};
            const bool clients = esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0;
            if (!clients && now_us - sta_up_since > 10LL * 1000 * 1000 && setup_connection_failed_) {
                LeaveSetup();
            }
        } else if (!st.sta_connected) {
            sta_up_since = 0;
        }

        if (screen_ == Screen::kInfo && now_us - info_shown_us_ > kInfoTimeoutUs) {
            screen_ = Screen::kBoard;
            DrawCurrent();
        }

        if (now_us - last_battery_us_ >= kFetchIntervalUs) {
            last_battery_us_ = now_us;
            const ZectrixPowerSnapshot p = board_.ReadPowerSnapshot();
            battery_percent_ = (p.battery_valid && !p.charge.power_present) ? p.battery_percent : -1;
        }

        if (screen_ == Screen::kSetup || screen_ == Screen::kInfo) continue;

        // Once a minute: fetch arrivals.
        if (now_us - last_fetch_us_ >= kFetchIntervalUs) {
            last_fetch_us_ = now_us;
            RequestFetch();
        }

        // Automatic page change.
        int cycle, pages;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cycle = cfg_.cycle;
            pages = hk::PageCount(cfg_);
        }
        bool redraw = redraw_.exchange(false);
        if (config_changed_.exchange(false)) {
            page_ = 0;
            page_shown_us_ = now_us;
            redraw = true;
        }
        if (cycle > 0 && pages > 1 && now_us - page_shown_us_ >= cycle * 1000000LL) {
            page_ = (page_ + 1) % pages;
            page_shown_us_ = now_us;
            redraw = true;
        }
        // Clock in the header and minutes counting down.
        if (net::TimeValid()) {
            const int minute = static_cast<int>(time(nullptr) / 60);
            if (minute != last_drawn_minute_) {
                last_drawn_minute_ = minute;
                redraw = true;
            }
        }
        if (redraw) DrawCurrent();
    }
}

}  // namespace

extern "C" void app_main(void) {
    static App app;
    app.Run();
}
