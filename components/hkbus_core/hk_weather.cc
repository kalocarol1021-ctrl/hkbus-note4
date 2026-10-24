#include "hk_weather.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cJSON.h"

namespace hk {

const char* const kHkoPlaces[] = {
    "香港天文台", "京士柏", "黃竹坑", "打鼓嶺", "流浮山", "大埔", "沙田", "屯門", "將軍澳",
    "西貢", "長洲", "赤鱲角", "青衣", "石崗", "荃灣可觀", "荃灣城門谷", "香港公園",
    "筲箕灣", "九龍城", "跑馬地", "黃大仙", "赤柱", "觀塘", "深水埗", "啟德跑道公園",
    "元朗公園", "大美督",
};
const int kHkoPlaceCount = sizeof(kHkoPlaces) / sizeof(kHkoPlaces[0]);

std::string HkoIconText(int icon) {
    switch (icon) {
        case 50: return "陽光充沛";
        case 51: return "間有陽光";
        case 52: return "短暫陽光";
        case 53: case 54: return "有驟雨";
        case 60: return "多雲";
        case 61: return "密雲";
        case 62: return "微雨";
        case 63: return "有雨";
        case 64: return "大雨";
        case 65: return "雷暴";
        case 70: case 71: case 72: case 73: case 74: case 75: return "天色良好";
        case 76: return "大致多雲";
        case 77: return "大致良好";
        case 80: return "大風";
        case 81: return "乾燥";
        case 82: return "潮濕";
        case 83: return "有霧";
        case 84: return "薄霧";
        case 85: return "煙霞";
        case 90: return "炎熱";
        case 91: return "溫暖";
        case 92: return "清涼";
        case 93: return "寒冷";
        default: return "";
    }
}

bool ParseHkoCurrent(const char* json, const std::string& place, WeatherNow* out) {
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) return false;
    WeatherNow w;
    const cJSON* temp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(temp, "data");
    const cJSON* chosen = nullptr;
    const cJSON* observatory = nullptr;
    const cJSON* first = nullptr;
    const cJSON* e = nullptr;
    cJSON_ArrayForEach(e, data) {
        const cJSON* p = cJSON_GetObjectItemCaseSensitive(e, "place");
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(e, "value");
        if (!cJSON_IsString(p) || !cJSON_IsNumber(v)) continue;
        if (first == nullptr) first = e;
        if (place == p->valuestring) chosen = e;
        if (std::strcmp(p->valuestring, "香港天文台") == 0) observatory = e;
    }
    if (chosen == nullptr) chosen = observatory != nullptr ? observatory : first;
    if (chosen != nullptr) {
        w.place = cJSON_GetObjectItemCaseSensitive(chosen, "place")->valuestring;
        w.temperature = static_cast<int>(
            std::lround(cJSON_GetObjectItemCaseSensitive(chosen, "value")->valuedouble));
        w.ok = true;
    }
    const cJSON* icons = cJSON_GetObjectItemCaseSensitive(root, "icon");
    if (cJSON_IsArray(icons) && cJSON_GetArraySize(icons) > 0) {
        const cJSON* i0 = cJSON_GetArrayItem(icons, 0);
        if (cJSON_IsNumber(i0)) {
            w.icon = i0->valueint;
            w.desc = HkoIconText(w.icon);
        }
    }
    cJSON_Delete(root);
    *out = w;
    return w.ok;
}

namespace {

struct Known {
    const char* code;
    const char* text;
    int priority;
    bool severe;
};

// Short labels that older people recognise; most important first.
constexpr Known kKnown[] = {
    {"TC10", "十號風球", 0, true},
    {"TC9", "九號風球", 1, true},
    {"TC8NE", "八號風球", 2, true},
    {"TC8SE", "八號風球", 2, true},
    {"TC8NW", "八號風球", 2, true},
    {"TC8SW", "八號風球", 2, true},
    {"WRAINB", "黑色暴雨", 3, true},
    {"WRAINR", "紅色暴雨", 4, true},
    {"WTMW", "海嘯警告", 5, true},
    {"TC3", "三號風球", 10, false},
    {"WRAINA", "黃色暴雨", 11, false},
    {"WTS", "雷暴警告", 12, false},
    {"WL", "山泥傾瀉", 13, false},
    {"WFNTSA", "新界北水浸", 14, false},
    {"TC1", "一號風球", 15, false},
    {"WMSGNL", "強烈季候風", 16, false},
    {"WHOT", "酷熱天氣", 17, false},
    {"WCOLD", "寒冷天氣", 18, false},
    {"WFROST", "霜凍警告", 19, false},
    {"WFIRER", "紅色火災危險", 20, false},
    {"WFIREY", "黃色火災危險", 21, false},
};

}  // namespace

bool ParseHkoWarnings(const char* json, std::vector<Warning>* out) {
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    std::vector<Warning> list;
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, root) {
        if (!cJSON_IsObject(entry)) continue;
        const cJSON* action = cJSON_GetObjectItemCaseSensitive(entry, "actionCode");
        if (cJSON_IsString(action) && std::strcmp(action->valuestring, "CANCEL") == 0) continue;
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(entry, "code");
        const char* c = cJSON_IsString(code) ? code->valuestring : entry->string;
        if (c == nullptr || std::strcmp(c, "CANCEL") == 0) continue;
        Warning w;
        for (const Known& k : kKnown) {
            if (std::strcmp(k.code, c) == 0) {
                w.text = k.text;
                w.priority = k.priority;
                w.severe = k.severe;
                break;
            }
        }
        if (w.text.empty()) {
            // Unknown code: use the Observatory's own name.
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(entry, "name");
            if (!cJSON_IsString(name)) continue;
            w.text = name->valuestring;
        }
        list.push_back(w);
    }
    cJSON_Delete(root);
    std::stable_sort(list.begin(), list.end(),
                     [](const Warning& a, const Warning& b) { return a.priority < b.priority; });
    *out = std::move(list);
    return true;
}

}  // namespace hk
