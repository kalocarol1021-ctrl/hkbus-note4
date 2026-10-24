// Board configuration and ETA logic. Pure C++ (+cJSON) so it can be unit
// tested on a PC; no ESP-IDF headers here.
//
// The per-operator filtering follows hkbus.app's open-source ETA library
// (https://github.com/hkbus/hk-bus-eta, GPL-3.0 project; logic re-implemented
// here, not copied).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hk {

// One operator's view of a route at a stop. Joint routes (e.g. KMB + Citybus
// 101) have one entry per operator.
struct CoStop {
    std::string co;     // "kmb", "ctb", "gmb", "nlb", "lrtfeeder"
    std::string bound;  // "O", "I", or "OI"/"IO" for some Citybus routes
    std::string stop;   // operator stop id
};

struct Item {
    std::string route;          // "1A", "N293", "11M"
    std::string service_type;   // KMB service type, usually "1"
    int seq = 0;                // 0-based stop index on the route (hkbus.app)
    std::string gtfs;           // GMB route id (gtfsId)
    std::string nlb;            // NLB route id
    std::vector<CoStop> cos;
    std::string dest;           // label shown after "往"
    std::string name;           // stop name label
};

// A daily time window in minutes after midnight (HKT). from > to wraps
// past midnight, e.g. 22:00-02:00.
struct TimeRange {
    int from = 0;
    int to = 0;
};

struct Config {
    std::string title;          // header text; empty = default
    int rows = 4;               // rows per page: 3 or 4
    int cycle = 0;              // auto page change every N seconds, 0 = off
    bool hours_on = false;      // only show/update during `hours`
    std::vector<TimeRange> hours;  // up to 3 windows
    std::vector<Item> items;
};

// Whether the board should be showing arrivals at this minute of the day.
bool IsShowTime(const Config& config, int minute_of_day);
// Start of the next display window after `minute_of_day`, or -1.
int NextShowStart(const Config& config, int minute_of_day);

bool ParseConfig(const char* json, Config* out, std::string* error);
std::string SerializeConfig(const Config& config);

// ---- Time ----------------------------------------------------------------
// "2026-10-24T17:20:15+08:00", "...Z", "...123+08:00" and
// "2026-10-24 17:20:00" (assumed Hong Kong time). Returns false on failure.
bool ParseIsoTime(const char* text, int64_t* epoch_out);
// Hong Kong local wall-clock for an epoch (HKT = UTC+8, no DST).
void HkClock(int64_t epoch, int* hour, int* minute);
// Minutes until `eta`, rounded the same way hkbus.app does.
int MinutesUntil(int64_t eta, int64_t now);

// ---- ETA fetching ---------------------------------------------------------
struct HttpRequest {
    std::string url;
    std::string post_body;  // empty = GET
    size_t co_index = 0;    // index into Item::cos
};
std::vector<HttpRequest> BuildRequests(const Item& item);

struct Eta {
    int64_t time = 0;       // epoch seconds; 0 = no time, remark only
    std::string remark;
    std::string co;
};

// Parse one operator response. `now` is only needed for MTR bus (relative
// seconds). Returns false if the body is not the expected JSON.
bool ParseEtaResponse(const Item& item, const CoStop& cs, const char* body,
                      int64_t now, std::vector<Eta>* out);

// Merge operator results the way hkbus.app does: drop remark-only rows when
// any real time exists, sort ascending, drop departed buses.
void MergeEtas(std::vector<Eta>* etas, int64_t now);

}  // namespace hk
