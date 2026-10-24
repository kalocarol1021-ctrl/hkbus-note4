// PC-side tests for the parsing logic and a screen preview renderer.
//   cmake -S test -B build-host && cmake --build build-host
//   ./build-host/hk_host_test test/fixtures
//   ./build-host/hk_preview main/fonts out.pbm [config.json] [3|4]
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "hk_model.h"
#include "hk_screen.h"
#include "hk_weather.h"

using namespace hk;

static int g_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

static std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static int64_t T(const char* iso) {
    int64_t t = 0;
    CHECK(ParseIsoTime(iso, &t));
    return t;
}

static Item MakeItem(const char* route, std::vector<CoStop> cos, int seq) {
    Item it;
    it.route = route;
    it.service_type = "1";
    it.seq = seq;
    it.cos = std::move(cos);
    return it;
}

static void TestTime() {
    CHECK(T("1970-01-01T08:00:00+08:00") == 0);
    CHECK(T("2026-10-24T17:13:00+08:00") == T("2026-10-24T09:13:00Z"));
    CHECK(T("2026-10-24 17:13:00") == T("2026-10-24T17:13:00+08:00"));
    CHECK(T("2026-10-24T17:13:10.123+08:00") == T("2026-10-24T17:13:10+08:00"));
    int h, m;
    HkClock(T("2026-10-24T23:59:00+08:00"), &h, &m);
    CHECK(h == 23 && m == 59);
    CHECK(MinutesUntil(T("2026-10-24T17:15:40+08:00"), T("2026-10-24T17:13:05+08:00")) == 3);
    CHECK(MinutesUntil(T("2026-10-24T17:13:20+08:00"), T("2026-10-24T17:13:05+08:00")) == 0);
}

static void TestParsers(const std::string& dir) {
    const int64_t now = T("2026-10-24T17:13:05+08:00");
    {
        Item it = MakeItem("1", {{"kmb", "O", "D360B3EBF155C15A"}}, 3);
        std::vector<Eta> e;
        CHECK(ParseEtaResponse(it, it.cos[0], ReadFile(dir + "/kmb_1.json").c_str(), now, &e));
        MergeEtas(&e, now);
        CHECK(e.size() == 3);
        CHECK(MinutesUntil(e[0].time, now) == 3);
        CHECK(MinutesUntil(e[1].time, now) == 11);
        CHECK(e[2].remark == "原定班次");
    }
    {
        Item it = MakeItem("1", {{"kmb", "O", "D360B3EBF155C15A"}}, 3);
        std::vector<Eta> e;
        CHECK(ParseEtaResponse(it, it.cos[0], ReadFile(dir + "/kmb_none.json").c_str(), now, &e));
        MergeEtas(&e, now);
        CHECK(e.size() == 1 && e[0].time == 0 && e[0].remark == "最後班次已過");
    }
    {
        // Joint route 101: KMB (inbound) + Citybus (outbound), merged.
        Item it = MakeItem("101", {{"kmb", "I", "CEFDCD673C8E07B4"}, {"ctb", "O", "001081"}}, 3);
        std::vector<Eta> e;
        CHECK(ParseEtaResponse(it, it.cos[0], ReadFile(dir + "/kmb_101.json").c_str(), now, &e));
        CHECK(ParseEtaResponse(it, it.cos[1], ReadFile(dir + "/ctb_101.json").c_str(), now, &e));
        MergeEtas(&e, now);
        CHECK(e.size() == 3);
        CHECK(e[0].co == "kmb" && MinutesUntil(e[0].time, now) == 0);
        CHECK(e[1].co == "ctb" && MinutesUntil(e[1].time, now) == 6);
        CHECK(e[2].co == "kmb");
        const auto reqs = BuildRequests(it);
        CHECK(reqs.size() == 2);
        CHECK(reqs[0].url == "https://data.etabus.gov.hk/v1/transport/kmb/eta/CEFDCD673C8E07B4/101/1");
        CHECK(reqs[1].url == "https://rt.data.gov.hk/v2/transport/citybus/eta/CTB/001081/101");
    }
    {
        Item it = MakeItem("1", {{"gmb", "I", "20000026"}}, 3);
        it.gtfs = "2006408";
        std::vector<Eta> e;
        CHECK(ParseEtaResponse(it, it.cos[0], ReadFile(dir + "/gmb_1.json").c_str(), now, &e));
        MergeEtas(&e, now);
        CHECK(e.size() == 2);
        CHECK(MinutesUntil(e[0].time, now) == 2);
        CHECK(BuildRequests(it)[0].url == "https://data.etagmb.gov.hk/eta/route-stop/2006408/20000026");
    }
    {
        Item it = MakeItem("1", {{"nlb", "O", "4"}}, 3);
        it.nlb = "1";
        std::vector<Eta> e;
        CHECK(ParseEtaResponse(it, it.cos[0], ReadFile(dir + "/nlb_1.json").c_str(), now, &e));
        MergeEtas(&e, now);
        CHECK(e.size() == 2);
        CHECK(MinutesUntil(e[0].time, now) == 8);
        CHECK(e[0].remark.empty() && e[1].remark == "預定班次");
    }
    {
        Item it = MakeItem("K18", {{"lrtfeeder", "O", "K18-D020"}}, 1);
        std::vector<Eta> e;
        CHECK(ParseEtaResponse(it, it.cos[0], ReadFile(dir + "/mtr_bus_K18.json").c_str(), now, &e));
        MergeEtas(&e, now);
        CHECK(e.size() == 3);
        CHECK(MinutesUntil(e[0].time, now) == 0);
        CHECK(MinutesUntil(e[1].time, now) == 21);
        const auto reqs = BuildRequests(it);
        CHECK(reqs.size() == 1 && reqs[0].post_body == "{\"language\":\"zh\",\"routeName\":\"K18\"}");
    }
    {
        // Garbage in, no crash.
        Item it = MakeItem("1", {{"kmb", "O", "X"}}, 0);
        std::vector<Eta> e;
        CHECK(!ParseEtaResponse(it, it.cos[0], "<html>502</html>", now, &e));
        CHECK(ParseEtaResponse(it, it.cos[0], "{\"data\":[]}", now, &e) && e.empty());
    }
}

static void TestConfig() {
    const char* json =
        "{\"title\":\"樂富\",\"rows\":3,\"hours\":{\"on\":true,\"ranges\":[{\"from\":\"07:00\",\"to\":\"10:00\"},{\"from\":\"22:00\",\"to\":\"01:00\"},{\"from\":\"bad\",\"to\":\"x\"}]},"
        "\"items\":[{\"route\":\"101\",\"st\":\"1\",\"seq\":3,\"co\":[\"kmb\",\"ctb\"],"
        "\"bound\":{\"kmb\":\"I\",\"ctb\":\"O\"},\"stop\":{\"kmb\":\"AB\",\"ctb\":\"001081\"},"
        "\"dest\":\"觀塘\",\"name\":\"堅彌地城\"},{\"route\":\"\",\"co\":[\"kmb\"]}]}";
    Config c;
    std::string err;
    CHECK(ParseConfig(json, &c, &err));
    CHECK(c.title == "樂富" && c.rows == 3 && c.hours_on && c.hours.size() == 2);
    CHECK(IsShowTime(c, 7 * 60) && IsShowTime(c, 9 * 60 + 59) && !IsShowTime(c, 10 * 60));
    CHECK(IsShowTime(c, 23 * 60) && IsShowTime(c, 30) && !IsShowTime(c, 60));
    CHECK(NextShowStart(c, 12 * 60) == 22 * 60 && NextShowStart(c, 2 * 60) == 7 * 60);
    {
        // 1.0.0 "night pause" config is migrated to a display window.
        Config old;
        CHECK(ParseConfig("{\"night\":{\"on\":true,\"from\":\"01:00\",\"to\":\"05:30\"},\"items\":[]}", &old, &err));
        CHECK(old.hours_on && old.hours.size() == 1 && !IsShowTime(old, 120) && IsShowTime(old, 6 * 60));
        Config none;
        CHECK(ParseConfig("{\"items\":[]}", &none, &err) && !none.hours_on && IsShowTime(none, 120));
    }
    CHECK(c.items.size() == 1 && c.items[0].cos.size() == 2 && c.items[0].cos[1].stop == "001081");
    Config c2;
    CHECK(ParseConfig(SerializeConfig(c).c_str(), &c2, &err));
    CHECK(SerializeConfig(c) == SerializeConfig(c2));
    CHECK(!ParseConfig("not json", &c2, &err));
}

static void TestWeather(const std::string& dir) {
    WeatherNow w;
    CHECK(ParseHkoCurrent(ReadFile(dir + "/hko_rhrread.json").c_str(), "大埔", &w));
    CHECK(w.place == "大埔" && w.temperature == 24 && w.icon == 62 && w.desc == "微雨");
    CHECK(ParseHkoCurrent(ReadFile(dir + "/hko_rhrread.json").c_str(), "不存在", &w));
    CHECK(w.place == "香港天文台" && w.temperature == 25);
    CHECK(!ParseHkoCurrent("oops", "大埔", &w));

    std::vector<Warning> ws;
    CHECK(ParseHkoWarnings(ReadFile(dir + "/hko_warnsum.json").c_str(), &ws));
    CHECK(ws.size() == 3);  // cancelled fire warning dropped
    CHECK(ws[0].text == "八號風球" && ws[0].severe);
    CHECK(ws[1].text == "黃色暴雨" && !ws[1].severe);
    CHECK(ws[2].text == "雷暴警告");
    CHECK(ParseHkoWarnings(ReadFile(dir + "/hko_warnsum_empty.json").c_str(), &ws) && ws.empty());

    Config c;
    std::string err;
    CHECK(ParseConfig("{\"items\":[]}", &c, &err) && c.weather_on && c.weather_place == "香港天文台");
    CHECK(ParseConfig("{\"weather\":{\"on\":true,\"place\":\"大埔\"},\"items\":[]}", &c, &err));
    CHECK(c.weather_place == "大埔");
    Config c2;
    CHECK(ParseConfig(SerializeConfig(c).c_str(), &c2, &err) && c2.weather_place == "大埔" && c2.weather_on);
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "test/fixtures";
    TestTime();
    TestParsers(dir);
    TestConfig();
    TestWeather(dir);
    if (g_fail == 0) std::printf("all tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
