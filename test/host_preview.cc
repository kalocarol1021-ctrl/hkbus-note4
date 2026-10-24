// Renders the e-paper screens on a PC so layouts can be checked without the
// device. Output is a PBM image (any image viewer, or convert to PNG).
//   hk_preview FONT_DIR OUT_PREFIX [config.json]
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "hk_model.h"
#include "hk_screen.h"

using namespace hk;

static std::vector<std::string> g_blobs;  // keep font data alive

static Font LoadFont(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    g_blobs.push_back(ss.str());
    Font font;
    if (!font.Load(reinterpret_cast<const uint8_t*>(g_blobs.back().data()), g_blobs.back().size())) {
        std::fprintf(stderr, "cannot load %s\n", path.c_str());
    }
    return font;
}

static void WritePbm(const Framebuffer& fb, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    f << "P4\n" << Framebuffer::kWidth << " " << Framebuffer::kHeight << "\n";
    // PBM: 1 = black, our buffer: 1 = white -> invert.
    for (size_t i = 0; i < Framebuffer::kBytes; ++i) f.put(static_cast<char>(~fb.data()[i]));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s FONT_DIR OUT_PREFIX [config.json]\n", argv[0]);
        return 2;
    }
    g_blobs.reserve(8);
    const std::string dir = argv[1];
    const std::string out = argv[2];
    Fonts fonts;
    fonts.s20 = LoadFont(dir + "/cjk20.bin");
    fonts.m26 = LoadFont(dir + "/cjk26.bin");
    fonts.l32 = LoadFont(dir + "/cjk32.bin");
    fonts.n44 = LoadFont(dir + "/num44.bin");
    fonts.n58 = LoadFont(dir + "/num58.bin");
    fonts.n44.SetFallback(&fonts.l32);
    fonts.n58.SetFallback(&fonts.l32);

    const char* sample =
        "{\"title\":\"樂富中心\",\"rows\":4,\"items\":["
        "{\"route\":\"1\",\"co\":[\"kmb\"],\"bound\":{\"kmb\":\"O\"},\"stop\":{\"kmb\":\"A\"},\"dest\":\"尖沙咀碼頭\",\"name\":\"摩士公園\"},"
        "{\"route\":\"101\",\"co\":[\"kmb\",\"ctb\"],\"bound\":{\"kmb\":\"I\",\"ctb\":\"O\"},\"stop\":{\"kmb\":\"A\",\"ctb\":\"B\"},\"dest\":\"觀塘(裕民坊)\",\"name\":\"堅彌地城海旁\"},"
        "{\"route\":\"11M\",\"co\":[\"gmb\"],\"bound\":{\"gmb\":\"O\"},\"stop\":{\"gmb\":\"C\"},\"gtfs\":\"1\",\"dest\":\"坪石\",\"name\":\"黃大仙站\"},"
        "{\"route\":\"N293\",\"co\":[\"kmb\"],\"bound\":{\"kmb\":\"O\"},\"stop\":{\"kmb\":\"D\"},\"dest\":\"將軍澳(寶琳)\",\"name\":\"旺角(洗衣街)\"},"
        "{\"route\":\"2X\",\"co\":[\"kmb\"],\"bound\":{\"kmb\":\"O\"},\"stop\":{\"kmb\":\"E\"},\"dest\":\"深水埗(欽州街)\",\"name\":\"摩士公園\"}"
        "]}";
    std::string json = sample;
    if (argc > 3) {
        std::ifstream f(argv[3]);
        std::stringstream ss;
        ss << f.rdbuf();
        json = ss.str();
    }
    Config cfg;
    std::string err;
    if (!ParseConfig(json.c_str(), &cfg, &err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return 1;
    }

    int64_t now = 0;
    ParseIsoTime("2026-10-24T17:13:05+08:00", &now);
    BoardView view;
    view.config = &cfg;
    view.now = now;
    view.rows.resize(cfg.items.size());
    const int mins[][3] = {{3, 11, 23}, {0, 6, 0}, {12, 0, 0}, {0, 0, 0}, {45, 0, 0}};
    for (size_t i = 0; i < view.rows.size(); ++i) {
        RowState& r = view.rows[i];
        r.loaded = true;
        r.updated = now - 20;
        if (i == 2) r.updated = now - 400;  // stale row
        if (i == 3) {
            r.remark = "最後班次已過";
            continue;
        }
        const int* m = mins[i % 5];
        for (int k = 0; k < 3; ++k) {
            if (k > 0 && m[k] == 0) break;
            r.times.push_back(now + m[k] * 60);
        }
    }

    Framebuffer fb;
    for (int rows : {4, 3}) {
        cfg.rows = rows;
        view.page = 0;
        DrawBoard(fb, fonts, view);
        WritePbm(fb, out + "_board" + std::to_string(rows) + ".pbm");
    }
    cfg.rows = 4;
    view.page = 1;
    view.battery_percent = 12;
    view.network_ok = false;
    DrawBoard(fb, fonts, view);
    WritePbm(fb, out + "_board4_p2.pbm");

    // These mirror the screens composed in main/app_main.cc.
    DrawMessage(fb, fonts, "設定 Wi-Fi",
                {"掃描右邊 QR Code", "或手機連接 Wi-Fi：", "HKBus-1A2B", "密碼：40731958",
                 "再開啟 192.168.4.1"},
                "WIFI:T:WPA;S:HKBus-1A2B;P:40731958;;", "掃描連接");
    WritePbm(fb, out + "_setup.pbm");

    DrawMessage(fb, fonts, "未設定車站",
                {"用連接同一 Wi-Fi 的", "手機掃描右邊 QR Code，", "或開啟以下網址：", "192.168.1.57",
                 "設定碼：7KX2PQ"},
                "http://192.168.1.57/?k=7KX2PQ", "掃描開啟設定頁");
    WritePbm(fb, out + "_empty.pbm");

    DrawMessage(fb, fonts, "設定資料 v1.1.0",
                {"Wi-Fi：MyHome", "網址：192.168.1.57", "設定碼：7KX2PQ", "音量＋：重設 Wi-Fi",
                 "Home 鍵：返回"},
                "http://192.168.1.57/?k=7KX2PQ", "掃描開啟設定頁");
    WritePbm(fb, out + "_info.pbm");

    DrawMessage(fb, fonts, "樂富中心",
                {"休息中，暫停更新", "16:00 恢復顯示", "", "按 Home 鍵臨時顯示 10 分鐘"});
    WritePbm(fb, out + "_rest.pbm");
    std::printf("ok\n");
    return 0;
}
