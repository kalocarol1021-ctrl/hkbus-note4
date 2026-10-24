// Over-the-air firmware update from the project's GitHub Pages site.
//
// ota.json (written by .github/workflows/release.yml):
//   {"version":"1.2.0","url":"https://…/hkbus-note4-app.bin",
//    "size":2850000,"sha256":"…","notes":"…"}
// The new image is downloaded into the inactive slot, its SHA-256 checked
// against the manifest, and only then selected for the next boot. With
// rollback enabled the bootloader returns to the old slot if the new
// firmware never marks itself valid.
#pragma once

#include <functional>
#include <string>

#include "esp_err.h"

namespace ota {

struct Manifest {
    bool ok = false;
    std::string error;
    std::string version;
    std::string url;
    std::string sha256;   // lowercase hex
    std::string notes;
    int size = 0;
};

// Fetch and parse the manifest at CONFIG_HKBUS_UPDATE_URL.
Manifest Check();

// <0 if a < b, 0 if equal, >0 if a > b ("1.10.0" > "1.9.3").
int CompareVersions(const std::string& a, const std::string& b);

// Download, verify and stage the update. progress(0..100) is called as data
// arrives. On ESP_OK the caller restarts the device.
esp_err_t Install(const Manifest& m, const std::function<void(int)>& progress,
                  std::string* error);

// First boot of a new image: call once the firmware has proved it runs.
void MarkRunningValid();

}  // namespace ota
