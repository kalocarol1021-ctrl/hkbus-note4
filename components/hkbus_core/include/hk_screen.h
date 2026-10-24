// Screen layouts. Pure drawing code: given fonts, configuration and ETA data,
// paint a Framebuffer. Shared by the firmware and the PC preview tool.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hk_fb.h"
#include "hk_font.h"
#include "hk_model.h"

namespace hk {

struct Fonts {
    Font s20;   // small CJK
    Font m26;   // medium CJK
    Font l32;   // large CJK
    Font n44;   // big digits/latin (route numbers, minutes)
    Font n58;   // bigger digits/latin (3-rows layout)
};

// Live state of one configured row.
struct RowState {
    bool loaded = false;          // at least one successful fetch
    bool failed = false;          // last fetch failed
    std::vector<int64_t> times;   // upcoming arrivals (epoch seconds)
    std::string remark;           // operator message when no times
    int64_t updated = 0;          // epoch of the last successful fetch
};

// Arrival data older than this is not shown (it would count down to wrong
// times while the network is down).
constexpr int64_t kStaleAfterSeconds = 180;

struct BoardView {
    const Config* config = nullptr;
    int page = 0;                       // 0-based
    std::vector<RowState> rows;         // same length as config->items
    int64_t now = 0;                    // epoch seconds; 0 = clock unknown
    bool network_ok = true;
    int battery_percent = -1;           // <0 = hide (USB powered)
};

int PageCount(const Config& config);

void DrawBoard(Framebuffer& fb, const Fonts& fonts, const BoardView& view);

// Generic message screen: title bar, a few lines of text and an optional
// QR code on the right (e.g. Wi-Fi join code or the settings URL).
void DrawMessage(Framebuffer& fb, const Fonts& fonts, const std::string& title,
                 const std::vector<std::string>& lines,
                 const std::string& qr_text = "",
                 const std::string& qr_caption = "");

// Draws a QR code with its top-left at (x, y), each module `scale` pixels.
// Returns the drawn size in pixels or 0 on failure.
int DrawQr(Framebuffer& fb, int x, int y, const std::string& text, int scale);

}  // namespace hk
