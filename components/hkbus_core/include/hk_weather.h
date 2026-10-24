// Hong Kong Observatory open data: current temperature / weather and the
// warning summary. Pure C++ (+cJSON) like hk_model.
//   https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=tc
//   https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=warnsum&lang=tc
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hk {

constexpr const char* kHkoCurrentUrl =
    "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=tc";
constexpr const char* kHkoWarningsUrl =
    "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=warnsum&lang=tc";

// Temperature stations reported in rhrread, for the settings page.
extern const char* const kHkoPlaces[];
extern const int kHkoPlaceCount;

struct WeatherNow {
    bool ok = false;
    std::string place;     // station actually used
    int temperature = 0;   // °C
    int icon = 0;          // HKO icon number (50-93), 0 = none
    std::string desc;      // short Chinese text for the icon
};

struct Warning {
    std::string text;      // short label, e.g. "八號風球", "黃色暴雨"
    int priority = 99;     // lower = more important
    bool severe = false;   // signal 8+, red/black rain: highlight
};

// Short Chinese description of an HKO weather icon ("多雲", "大雨"…).
std::string HkoIconText(int icon);

// `place` falls back to 香港天文台, then to the first station, if missing.
bool ParseHkoCurrent(const char* json, const std::string& place, WeatherNow* out);

// Active warnings sorted by importance (cancelled ones are skipped).
bool ParseHkoWarnings(const char* json, std::vector<Warning>* out);

}  // namespace hk
