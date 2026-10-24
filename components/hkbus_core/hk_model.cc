#include "hk_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>

#include "cJSON.h"

namespace hk {
namespace {

// ---- small cJSON helpers --------------------------------------------------

std::string Str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring != nullptr) return v->valuestring;
    if (cJSON_IsNumber(v)) {
        char buf[32];
        if (v->valuedouble == static_cast<double>(static_cast<long long>(v->valuedouble))) {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v->valuedouble));
        } else {
            std::snprintf(buf, sizeof(buf), "%g", v->valuedouble);
        }
        return buf;
    }
    return "";
}

// Numbers sometimes arrive as strings ("1") and sometimes as numbers (1).
bool Num(const cJSON* obj, const char* key, double* out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        *out = v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring != nullptr && v->valuestring[0] != '\0') {
        char* end = nullptr;
        const double d = std::strtod(v->valuestring, &end);
        if (end != v->valuestring) {
            *out = d;
            return true;
        }
    }
    return false;
}

bool Truthy(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v) && v->valuestring) return std::strcmp(v->valuestring, "1") == 0 ||
                                                    std::strcmp(v->valuestring, "true") == 0;
    return false;
}

int ParseHm(const std::string& s, int fallback) {
    int h = 0, m = 0;
    if (std::sscanf(s.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
        return h * 60 + m;
    }
    return fallback;
}

std::string Hm(int minutes) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60, minutes % 60);
    return buf;
}

// URL path segment escaping (route numbers and stop ids are plain ASCII but
// be safe anyway).
std::string Esc(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// ---- Operator specific parsers -------------------------------------------

// KMB / Long Win: one call per stop+route+service type. Keep the requested
// direction and, among those, the ETAs whose stop sequence is nearest to our
// stop (route data and live data are sometimes off by one).
bool ParseKmbOrCtb(const Item& item, const CoStop& cs, const cJSON* root,
                   std::vector<Eta>* out) {
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) return false;
    const int want_seq = item.seq + 1;  // API sequences are 1-based

    struct Row { const cJSON* node; int seq; };
    std::vector<Row> rows;
    const cJSON* e = nullptr;
    cJSON_ArrayForEach(e, data) {
        const std::string dir = Str(e, "dir");
        if (dir.empty() || cs.bound.find(dir) == std::string::npos) continue;
        double seq = 0;
        if (!Num(e, "seq", &seq)) continue;
        rows.push_back({e, static_cast<int>(seq)});
    }
    if (rows.empty()) return true;

    int best = rows.front().seq;
    for (const Row& r : rows) {
        if (std::abs(r.seq - want_seq) < std::abs(best - want_seq)) best = r.seq;
    }
    for (const Row& r : rows) {
        if (r.seq != best) continue;
        Eta eta;
        eta.co = cs.co;
        eta.remark = Str(r.node, "rmk_tc");
        const std::string t = Str(r.node, "eta");
        if (!t.empty() && !ParseIsoTime(t.c_str(), &eta.time)) eta.time = 0;
        out->push_back(eta);
    }
    return true;
}

// Green minibus: route_seq 1 = outbound, 2 = inbound; stop_seq is 1-based.
bool ParseGmb(const Item& item, const CoStop& cs, const cJSON* root,
              std::vector<Eta>* out) {
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) return false;
    const int want_route_seq = cs.bound == "I" ? 2 : 1;
    const cJSON* e = nullptr;
    cJSON_ArrayForEach(e, data) {
        double route_seq = 0, stop_seq = 0;
        if (!Num(e, "route_seq", &route_seq) || !Num(e, "stop_seq", &stop_seq)) continue;
        if (static_cast<int>(route_seq) != want_route_seq) continue;
        if (static_cast<int>(stop_seq) != item.seq + 1) continue;
        if (Truthy(e, "enabled")) {
            const cJSON* list = cJSON_GetObjectItemCaseSensitive(e, "eta");
            const cJSON* x = nullptr;
            cJSON_ArrayForEach(x, list) {
                Eta eta;
                eta.co = cs.co;
                eta.remark = Str(x, "remarks_tc");
                const std::string t = Str(x, "timestamp");
                if (t.empty() || !ParseIsoTime(t.c_str(), &eta.time)) continue;
                out->push_back(eta);
            }
        } else {
            Eta eta;
            eta.co = cs.co;
            eta.remark = Str(e, "description_tc");
            out->push_back(eta);
        }
    }
    return true;
}

// New Lantao Bus.
bool ParseNlb(const CoStop& cs, const cJSON* root, std::vector<Eta>* out) {
    if (!cJSON_IsObject(root)) return false;
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "estimatedArrivals");
    const size_t before = out->size();
    const cJSON* e = nullptr;
    cJSON_ArrayForEach(e, list) {
        const std::string t = Str(e, "estimatedArrivalTime");
        Eta eta;
        eta.co = cs.co;
        if (t.empty() || !ParseIsoTime(t.c_str(), &eta.time)) continue;
        const bool scheduled = Str(e, "departed") != "1" || Str(e, "noGPS") == "1";
        if (scheduled) eta.remark = "預定班次";
        out->push_back(eta);
    }
    if (out->size() == before) {
        const std::string msg = Str(root, "message");
        if (!msg.empty()) out->push_back({0, msg, cs.co});
    }
    return true;
}

// MTR feeder bus: whole-route schedule with seconds-from-now.
bool ParseMtrBus(const CoStop& cs, const cJSON* root, int64_t now,
                 std::vector<Eta>* out) {
    const cJSON* stops = cJSON_GetObjectItemCaseSensitive(root, "busStop");
    if (!cJSON_IsArray(stops)) return false;
    const size_t before = out->size();
    const cJSON* s = nullptr;
    cJSON_ArrayForEach(s, stops) {
        if (Str(s, "busStopId") != cs.stop) continue;
        const std::string stop_remark = Str(s, "busStopRemark");
        const cJSON* b = nullptr;
        cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(s, "bus")) {
            std::string secs = Str(b, "arrivalTimeInSecond");
            if (secs == "108000") secs = Str(b, "departureTimeInSecond");
            if (secs.empty()) continue;
            Eta eta;
            eta.co = cs.co;
            eta.time = now + std::atoll(secs.c_str());
            eta.remark = !stop_remark.empty() ? stop_remark : Str(b, "busRemark");
            if (eta.remark.empty() && Str(b, "isScheduled") == "1") eta.remark = "預定班次";
            out->push_back(eta);
        }
    }
    if (out->size() == before) {
        const std::string title = Str(root, "routeStatusRemarkTitle");
        if (!title.empty()) out->push_back({0, title, cs.co});
    }
    return true;
}

}  // namespace

// ---- Config ---------------------------------------------------------------

bool ParseConfig(const char* json, Config* out, std::string* error) {
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr || !cJSON_IsObject(root)) {
        if (error) *error = "JSON 格式錯誤";
        cJSON_Delete(root);
        return false;
    }
    Config c;
    c.title = Str(root, "title");
    double rows = 4;
    if (Num(root, "rows", &rows)) c.rows = static_cast<int>(rows) == 3 ? 3 : 4;
    double cycle = 0;
    if (Num(root, "cycle", &cycle)) c.cycle = std::clamp(static_cast<int>(cycle), 0, 600);
    if (c.cycle > 0 && c.cycle < 10) c.cycle = 10;
    const cJSON* hours = cJSON_GetObjectItemCaseSensitive(root, "hours");
    if (cJSON_IsObject(hours)) {
        c.hours_on = Truthy(hours, "on");
        const cJSON* r = nullptr;
        cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(hours, "ranges")) {
            TimeRange tr;
            tr.from = ParseHm(Str(r, "from"), -1);
            tr.to = ParseHm(Str(r, "to"), -1);
            if (tr.from < 0 || tr.to < 0 || tr.from == tr.to) continue;
            c.hours.push_back(tr);
            if (c.hours.size() >= 3) break;
        }
        if (c.hours.empty()) c.hours_on = false;
    } else {
        // Version 1.0.0 had a single "night pause" window: show outside it.
        const cJSON* night = cJSON_GetObjectItemCaseSensitive(root, "night");
        if (cJSON_IsObject(night) && Truthy(night, "on")) {
            const int from = ParseHm(Str(night, "from"), -1);
            const int to = ParseHm(Str(night, "to"), -1);
            if (from >= 0 && to >= 0 && from != to) {
                c.hours_on = true;
                c.hours.push_back({to, from});
            }
        }
    }
    const cJSON* items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, items) {
        Item item;
        item.route = Str(it, "route");
        item.service_type = Str(it, "st");
        if (item.service_type.empty()) item.service_type = "1";
        double seq = 0;
        Num(it, "seq", &seq);
        item.seq = static_cast<int>(seq);
        item.gtfs = Str(it, "gtfs");
        item.nlb = Str(it, "nlb");
        item.dest = Str(it, "dest");
        item.name = Str(it, "name");
        const cJSON* cos = cJSON_GetObjectItemCaseSensitive(it, "co");
        const cJSON* bound = cJSON_GetObjectItemCaseSensitive(it, "bound");
        const cJSON* stop = cJSON_GetObjectItemCaseSensitive(it, "stop");
        const cJSON* co = nullptr;
        cJSON_ArrayForEach(co, cos) {
            if (!cJSON_IsString(co)) continue;
            CoStop cs;
            cs.co = co->valuestring;
            cs.bound = Str(bound, co->valuestring);
            cs.stop = Str(stop, co->valuestring);
            if (cs.stop.empty()) continue;
            item.cos.push_back(cs);
        }
        if (item.route.empty() || item.cos.empty()) continue;
        if (item.route.size() > 8 || item.dest.size() > 120 || item.name.size() > 120) continue;
        c.items.push_back(std::move(item));
        if (c.items.size() >= 24) break;
    }
    cJSON_Delete(root);
    *out = std::move(c);
    return true;
}

std::string SerializeConfig(const Config& c) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "title", c.title.c_str());
    cJSON_AddNumberToObject(root, "rows", c.rows);
    cJSON_AddNumberToObject(root, "cycle", c.cycle);
    cJSON* hours = cJSON_AddObjectToObject(root, "hours");
    cJSON_AddBoolToObject(hours, "on", c.hours_on);
    cJSON* ranges = cJSON_AddArrayToObject(hours, "ranges");
    for (const TimeRange& tr : c.hours) {
        cJSON* r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "from", Hm(tr.from).c_str());
        cJSON_AddStringToObject(r, "to", Hm(tr.to).c_str());
        cJSON_AddItemToArray(ranges, r);
    }
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    for (const Item& item : c.items) {
        cJSON* it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "route", item.route.c_str());
        cJSON_AddStringToObject(it, "st", item.service_type.c_str());
        cJSON_AddNumberToObject(it, "seq", item.seq);
        cJSON_AddStringToObject(it, "gtfs", item.gtfs.c_str());
        cJSON_AddStringToObject(it, "nlb", item.nlb.c_str());
        cJSON_AddStringToObject(it, "dest", item.dest.c_str());
        cJSON_AddStringToObject(it, "name", item.name.c_str());
        cJSON* cos = cJSON_AddArrayToObject(it, "co");
        cJSON* bound = cJSON_AddObjectToObject(it, "bound");
        cJSON* stop = cJSON_AddObjectToObject(it, "stop");
        for (const CoStop& cs : item.cos) {
            cJSON_AddItemToArray(cos, cJSON_CreateString(cs.co.c_str()));
            cJSON_AddStringToObject(bound, cs.co.c_str(), cs.bound.c_str());
            cJSON_AddStringToObject(stop, cs.co.c_str(), cs.stop.c_str());
        }
        cJSON_AddItemToArray(items, it);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

namespace {
bool InRange(const TimeRange& r, int m) {
    if (r.from < r.to) return m >= r.from && m < r.to;
    return m >= r.from || m < r.to;  // wraps midnight
}
}  // namespace

bool IsShowTime(const Config& c, int minute) {
    if (!c.hours_on || c.hours.empty()) return true;
    for (const TimeRange& r : c.hours) {
        if (InRange(r, minute)) return true;
    }
    return false;
}

int NextShowStart(const Config& c, int minute) {
    int best = -1, best_wait = 24 * 60 + 1;
    for (const TimeRange& r : c.hours) {
        int wait = r.from - minute;
        if (wait <= 0) wait += 24 * 60;
        if (wait < best_wait) {
            best_wait = wait;
            best = r.from;
        }
    }
    return best;
}

// ---- Time -----------------------------------------------------------------

bool ParseIsoTime(const char* s, int64_t* epoch) {
    int Y, M, D, h, m, sec = 0, n = 0;
    if (std::sscanf(s, "%4d-%2d-%2d%*1[T ]%2d:%2d%n", &Y, &M, &D, &h, &m, &n) != 5) return false;
    const char* p = s + n;
    if (*p == ':') {
        int k = 0;
        if (std::sscanf(p, ":%2d%n", &sec, &k) != 1) return false;
        p += k;
    }
    if (*p == '.') {  // fractional seconds
        ++p;
        while (*p >= '0' && *p <= '9') ++p;
    }
    int offset_min = 8 * 60;  // default: Hong Kong time
    if (*p == 'Z') {
        offset_min = 0;
    } else if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        if (std::sscanf(p + 1, "%2d:%2d", &oh, &om) < 1) return false;
        offset_min = (oh * 60 + om) * (*p == '-' ? -1 : 1);
    }
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return false;
    const int64_t days = DaysFromCivil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D));
    *epoch = days * 86400 + h * 3600 + m * 60 + sec - offset_min * 60;
    return true;
}

void HkClock(int64_t epoch, int* hour, int* minute) {
    int64_t local = (epoch + 8 * 3600) % 86400;
    if (local < 0) local += 86400;
    *hour = static_cast<int>(local / 3600);
    *minute = static_cast<int>((local / 60) % 60);
}

int MinutesUntil(int64_t eta, int64_t now) {
    return static_cast<int>(std::lround(static_cast<double>(eta - now) / 60.0));
}

// ---- ETA ------------------------------------------------------------------

std::vector<HttpRequest> BuildRequests(const Item& item) {
    std::vector<HttpRequest> out;
    for (size_t i = 0; i < item.cos.size(); ++i) {
        const CoStop& cs = item.cos[i];
        HttpRequest r;
        r.co_index = i;
        if (cs.co == "kmb") {
            r.url = "https://data.etabus.gov.hk/v1/transport/kmb/eta/" + Esc(cs.stop) + "/" +
                    Esc(item.route) + "/" + Esc(item.service_type);
        } else if (cs.co == "ctb") {
            r.url = "https://rt.data.gov.hk/v2/transport/citybus/eta/CTB/" + Esc(cs.stop) + "/" +
                    Esc(item.route);
        } else if (cs.co == "gmb") {
            if (item.gtfs.empty()) continue;
            r.url = "https://data.etagmb.gov.hk/eta/route-stop/" + Esc(item.gtfs) + "/" +
                    Esc(cs.stop);
        } else if (cs.co == "nlb") {
            if (item.nlb.empty()) continue;
            r.url = "https://rt.data.gov.hk/v2/transport/nlb/stop.php?action=estimatedArrivals"
                    "&routeId=" + Esc(item.nlb) + "&stopId=" + Esc(cs.stop) + "&language=zh";
        } else if (cs.co == "lrtfeeder") {
            r.url = "https://rt.data.gov.hk/v1/transport/mtr/bus/getSchedule";
            cJSON* body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "language", "zh");
            cJSON_AddStringToObject(body, "routeName", item.route.c_str());
            char* text = cJSON_PrintUnformatted(body);
            r.post_body = text != nullptr ? text : "";
            cJSON_free(text);
            cJSON_Delete(body);
        } else {
            continue;  // unsupported operator (trains/ferries)
        }
        out.push_back(r);
    }
    return out;
}

bool ParseEtaResponse(const Item& item, const CoStop& cs, const char* body,
                      int64_t now, std::vector<Eta>* out) {
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return false;
    bool ok = false;
    if (cs.co == "kmb" || cs.co == "ctb") ok = ParseKmbOrCtb(item, cs, root, out);
    else if (cs.co == "gmb") ok = ParseGmb(item, cs, root, out);
    else if (cs.co == "nlb") ok = ParseNlb(cs, root, out);
    else if (cs.co == "lrtfeeder") ok = ParseMtrBus(cs, root, now, out);
    cJSON_Delete(root);
    return ok;
}

void MergeEtas(std::vector<Eta>* etas, int64_t now) {
    const bool any_time = std::any_of(etas->begin(), etas->end(),
                                      [](const Eta& e) { return e.time != 0; });
    if (any_time) {
        etas->erase(std::remove_if(etas->begin(), etas->end(),
                                   [now](const Eta& e) {
                                       return e.time == 0 || MinutesUntil(e.time, now) < 0;
                                   }),
                    etas->end());
    }
    std::stable_sort(etas->begin(), etas->end(), [](const Eta& a, const Eta& b) {
        if (a.time == 0) return false;
        if (b.time == 0) return true;
        return a.time < b.time;
    });
}

}  // namespace hk
