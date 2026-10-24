// Wi-Fi station / set-up access point, captive DNS and clock sync.
#pragma once

#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace net {

struct Status {
    bool sta_connected = false;
    bool ap_active = false;
    std::string sta_ssid;   // configured home network
    std::string ip;         // station IP when connected
    int rssi = 0;
    std::string ap_ssid;    // set-up network name
    std::string ap_pass;    // set-up network password (shown on screen)
    bool time_valid = false;
};

struct ScanEntry {
    std::string ssid;
    int rssi;
    bool secure;
};

esp_err_t Init();
bool LoadCredentials(std::string* ssid, std::string* pass);
esp_err_t SaveCredentials(const std::string& ssid, const std::string& pass);

// Connect to the home network (keeps retrying in the background).
void StartStation(const std::string& ssid, const std::string& pass);
// Bring up the open set-up network "HKBus-XXXX" (station keeps running).
void StartSetupAp();
void StopSetupAp();
bool WaitConnected(TickType_t timeout);
Status GetStatus();
std::vector<ScanEntry> Scan();

// Clock: SNTP once connected; HTTP Date headers as a fallback.
void StartTimeSync();
bool TimeValid();
void SetTimeFromHttpDate(const char* http_date);

// Address of the device on its own set-up network.
inline constexpr const char* kSetupIp = "192.168.4.1";

}  // namespace net
