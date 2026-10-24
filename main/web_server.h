// Settings web page and JSON API served by the device itself.
#pragma once

#include <functional>
#include <string>

struct WebHooks {
    // Code shown on the device; required by every API call except from the
    // device's own (password-protected) set-up network.
    std::string access_code;
    // Whole device state as JSON (config, Wi-Fi, live arrival data).
    std::function<std::string()> state_json;
    // Validate + store + apply a new board configuration.
    std::function<bool(const std::string& json, std::string* error)> save_config;
    std::function<void()> refresh_now;
    // Current screen as a 1-bit BMP.
    std::function<std::string()> screen_bmp;
    // Firmware update: check returns JSON; install starts it in the background.
    std::function<std::string()> update_check;
    std::function<bool(std::string* error)> update_install;
    // New home Wi-Fi saved: the device restarts to use it.
    std::function<void(const std::string& ssid, const std::string& pass)> save_wifi;
};

namespace web {
bool Start(const WebHooks& hooks);
}  // namespace web
