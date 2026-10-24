#include "hk_screen.h"

#include <algorithm>
#include <cstdio>

#include "qrcodegen.h"

namespace hk {
namespace {

constexpr int kW = Framebuffer::kWidth;
constexpr int kH = Framebuffer::kHeight;
constexpr int kHeader = 36;
constexpr int kMargin = 6;

struct Layout {
    int row_h;
    int route_w;
    int eta_w;
    const Font* route_font;
    const Font* dest_font;
    const Font* stop_font;
    const Font* big_font;     // minutes
    const Font* unit_font;    // "分"
    const Font* next_font;    // following arrivals
    const Font* note_font;    // remarks / "即將到站"
};

Layout MakeLayout(const Fonts& f, int rows) {
    const int area = kH - kHeader;
    if (rows == 3) {
        return {area / 3, 124, 140, &f.n58, &f.l32, &f.m26, &f.n58, &f.m26, &f.s20, &f.m26};
    }
    return {area / 4, 104, 128, &f.n44, &f.m26, &f.s20, &f.n44, &f.s20, &f.s20, &f.s20};
}

std::string StripBrackets(const std::string& s) {
    // "觀塘(裕民坊)" -> "觀塘": keeps rows short; the bracket text rarely matters
    // to someone waiting at their own stop. Only used when it doesn't fit.
    const size_t a = s.find('(');
    const size_t b = s.find("（");
    const size_t cut = std::min(a, b);
    if (cut == std::string::npos || cut == 0) return s;
    return s.substr(0, cut);
}

// Draw right-aligned, return the left x.
int DrawRight(Framebuffer& fb, const Font& font, int right, int baseline,
              const std::string& text, bool black = true) {
    const int x = right - font.TextWidth(text);
    font.Draw(fb, x, baseline, text, black);
    return x;
}

void DrawHeader(Framebuffer& fb, const Fonts& fonts, const std::string& title,
                const std::string& right_text) {
    fb.FillRect(0, 0, kW, kHeader, true);
    const int baseline = (kHeader + fonts.m26.ascent()) / 2;
    int right = kW - 8;
    if (!right_text.empty()) right = DrawRight(fb, fonts.m26, right, baseline, right_text, false) - 12;
    const std::string t = fonts.m26.Ellipsize(title, right - 10);
    fonts.m26.Draw(fb, 10, baseline, t, false);
}

void DrawRouteBox(Framebuffer& fb, const Fonts& fonts, const Layout& L,
                  int x, int y, int w, int h, const Item& item) {
    const bool minibus = !item.cos.empty() && item.cos[0].co == "gmb";
    if (minibus) {
        // Minibus routes: outlined box so they are easy to tell apart.
        fb.FillRoundRect(x, y, w, h, 8, true);
        fb.FillRoundRect(x + 4, y + 4, w - 8, h - 8, 5, false);
    } else {
        fb.FillRoundRect(x, y, w, h, 8, true);
    }
    const Font* candidates[] = {L.route_font, &fonts.n44, &fonts.l32, &fonts.m26, &fonts.s20};
    const Font* font = candidates[0];
    for (const Font* f : candidates) {
        font = f;
        if (f->TextWidth(item.route) <= w - 12) break;
    }
    const int tw = font->TextWidth(item.route);
    // Centre on the cap height of digits (ascent of a CJK font is a bit taller
    // than Latin capitals, so use 0.8 of it for Latin-only fonts).
    const bool latin_font = font == &fonts.n44 || font == &fonts.n58;
    const int cap = latin_font ? font->ascent() * 74 / 100 : font->ascent() * 80 / 100;
    const int baseline = y + (h + cap) / 2;
    font->Draw(fb, x + (w - tw) / 2, baseline, item.route, minibus);
}

std::string JoinMinutes(const std::vector<int>& mins) {
    std::string out;
    for (size_t i = 0; i < mins.size(); ++i) {
        if (i) out += "、";
        out += std::to_string(mins[i]);
    }
    return out;
}

struct EtaExtent {
    int top_left;     // leftmost x used in the upper half of the row
    int bottom_left;  // leftmost x used in the lower half
};

EtaExtent DrawEta(Framebuffer& fb, const Fonts& fonts, const Layout& L, int right,
                  int y, int h, const RowState& row, int64_t now) {
    std::vector<int> mins;
    const bool stale = row.loaded && now > 0 && row.updated > 0 &&
                       now - row.updated > kStaleAfterSeconds;
    if (now > 0 && !stale) {
        for (int64_t t : row.times) {
            const int m = MinutesUntil(t, now);
            if (m >= 0) mins.push_back(m);
            if (mins.size() >= 3) break;
        }
    }
    const int mid = y + h / 2;

    if (mins.empty()) {
        std::string text;
        if (stale) text = "資料未更新";
        else if (!row.loaded) text = row.failed ? "連線失敗" : "載入中…";
        else if (!row.remark.empty()) text = row.remark;
        else text = "暫無班次";
        const Font& f = *L.note_font;
        // Up to two lines of remark text.
        const int max_w = L.eta_w;
        if (f.TextWidth(text) <= max_w) {
            const int x = DrawRight(fb, f, right, mid + f.ascent() / 2, text);
            return {x, x};
        } else {
            std::string first, rest;
            const char* p = text.c_str();
            int w = 0;
            while (*p) {
                const char* s = p;
                const int adv = f.Advance(NextCodepoint(&p));
                if (w + adv > max_w) { p = s; break; }
                w += adv;
                first.append(s, p - s);
            }
            rest = f.Ellipsize(p, max_w);
            const int gap = 4;
            const int top = mid - (2 * f.ascent() + gap) / 2;
            const int x1 = DrawRight(fb, f, right, top + f.ascent(), first);
            const int x2 = DrawRight(fb, f, right, top + 2 * f.ascent() + gap, rest);
            return {x1, x2};
        }
    }

    const Font& big = *L.big_font;
    const Font& unit = *L.unit_font;
    const Font& next = *L.next_font;
    const int digit_cap = big.ascent() * 74 / 100;
    const bool has_next = mins.size() > 1;
    const int block_h = digit_cap + (has_next ? 8 + next.ascent() : 0);
    const int top = mid - block_h / 2;
    const int big_base = top + digit_cap;

    EtaExtent ext{right, right};
    if (mins[0] == 0) {
        const Font& f = *L.dest_font;
        ext.top_left = DrawRight(fb, f, right, top + (digit_cap + f.ascent()) / 2, "即將到站");
    } else {
        int x = DrawRight(fb, unit, right, big_base, "分");
        ext.top_left = DrawRight(fb, big, x - 3, big_base, std::to_string(mins[0]));
    }
    if (has_next) {
        std::vector<int> rest(mins.begin() + 1, mins.end());
        ext.bottom_left = DrawRight(fb, next, right, big_base + 8 + next.ascent(),
                                    "之後 " + JoinMinutes(rest) + " 分");
    } else {
        ext.bottom_left = ext.top_left;
    }
    return ext;
}

}  // namespace

int PageCount(const Config& config) {
    const int rows = config.rows == 3 ? 3 : 4;
    const int n = static_cast<int>(config.items.size());
    return std::max(1, (n + rows - 1) / rows);
}

void DrawBoard(Framebuffer& fb, const Fonts& fonts, const BoardView& view) {
    fb.Clear();
    const Config& cfg = *view.config;
    const int rows = cfg.rows == 3 ? 3 : 4;
    const int pages = PageCount(cfg);
    const int page = std::clamp(view.page, 0, pages - 1);

    std::string right;
    if (view.now > 0) {
        int hh, mm;
        HkClock(view.now, &hh, &mm);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
        right = buf;
    }
    if (pages > 1) {
        right = std::to_string(page + 1) + "/" + std::to_string(pages) +
                (right.empty() ? "" : "  " + right);
    }
    std::string title = cfg.title.empty() ? "巴士到站時間" : cfg.title;
    if (!view.network_ok) title = "網絡中斷，稍後重試";
    DrawHeader(fb, fonts, title, right);

    const Layout L = MakeLayout(fonts, rows);
    const int start = page * rows;
    const int end = std::min(static_cast<int>(cfg.items.size()), start + rows);
    for (int i = start; i < end; ++i) {
        const Item& item = cfg.items[i];
        const int y = kHeader + (i - start) * L.row_h;
        if (i > start) fb.HLine(kMargin, y, kW - 2 * kMargin);

        const int box_h = L.row_h - 12;
        DrawRouteBox(fb, fonts, L, kMargin, y + 6, L.route_w, box_h, item);

        // Arrival times first: the text on the left may use whatever space
        // they leave free on each line.
        const RowState empty;
        const RowState& row = i < static_cast<int>(view.rows.size()) ? view.rows[i] : empty;
        const EtaExtent ext = DrawEta(fb, fonts, L, kW - kMargin - 2, y, L.row_h, row, view.now);

        // Destination and stop name.
        const int text_x = kMargin + L.route_w + 10;
        const int dest_w = ext.top_left - 10 - text_x;
        const int stop_w = ext.bottom_left - 10 - text_x;
        const Font& df = *L.dest_font;
        const Font& sf = *L.stop_font;
        const int gap = rows == 3 ? 10 : 7;
        const int block = df.ascent() + gap + sf.ascent();
        const int top = y + (L.row_h - block) / 2;
        const int dest_base = top + df.ascent();
        std::string dest = item.dest;
        if (df.TextWidth("往" + dest) > dest_w) dest = StripBrackets(dest);
        // Too long for the big font: use the next size down before cutting.
        const Font& dfont = df.TextWidth("往" + dest) <= dest_w ? df
                            : (&df == &fonts.l32 ? fonts.m26 : fonts.s20);
        const int after = dfont.Draw(fb, text_x, dest_base, "往", true);
        dfont.Draw(fb, after + 2, dest_base, dfont.Ellipsize(dest, dest_w - (after + 2 - text_x)));
        std::string stop = item.name;
        if (sf.TextWidth(stop) > stop_w) stop = StripBrackets(stop);
        sf.Draw(fb, text_x, top + block, sf.Ellipsize(stop, stop_w));
    }

    if (cfg.items.empty()) {
        const Font& f = fonts.m26;
        const char* msg = "未設定車站";
        f.Draw(fb, (kW - f.TextWidth(msg)) / 2, 150, msg);
    }

    if (view.battery_percent >= 0 && view.battery_percent <= 15) {
        // Low battery hint in the bottom-right corner.
        const std::string t = "電量低 " + std::to_string(view.battery_percent) + "%";
        const int w = fonts.s20.TextWidth(t) + 12;
        fb.FillRect(kW - w, kH - 26, w, 26, true);
        fonts.s20.Draw(fb, kW - w + 6, kH - 6, t, false);
    }
}

int DrawQr(Framebuffer& fb, int x, int y, const std::string& text, int scale) {
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    if (!qrcodegen_encodeText(text.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10,
                              qrcodegen_Mask_AUTO, true)) {
        return 0;
    }
    const int n = qrcodegen_getSize(qr);
    const int quiet = 2;
    const int px = (n + 2 * quiet) * scale;
    fb.FillRect(x, y, px, px, false);
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            if (qrcodegen_getModule(qr, c, r)) {
                fb.FillRect(x + (c + quiet) * scale, y + (r + quiet) * scale, scale, scale, true);
            }
        }
    }
    return px;
}

void DrawMessage(Framebuffer& fb, const Fonts& fonts, const std::string& title,
                 const std::vector<std::string>& lines, const std::string& qr_text,
                 const std::string& qr_caption) {
    fb.Clear();
    DrawHeader(fb, fonts, title, "");
    int text_right = kW - 12;
    if (!qr_text.empty()) {
        // Fit the code into a ~150 px square on the right.
        int scale = 5;
        int size = 0;
        while (scale >= 2) {
            uint8_t probe[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
            uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
            if (!qrcodegen_encodeText(qr_text.c_str(), tmp, probe, qrcodegen_Ecc_MEDIUM, 1, 10,
                                      qrcodegen_Mask_AUTO, true)) {
                break;
            }
            size = (qrcodegen_getSize(probe) + 4) * scale;
            if (size <= 160) break;
            --scale;
        }
        if (size > 0) {
            const int qx = kW - size - 8;
            const int qy = kHeader + 10;
            DrawQr(fb, qx, qy, qr_text, scale);
            if (!qr_caption.empty()) {
                const Font& f = fonts.s20;
                const std::string cap = f.Ellipsize(qr_caption, size + 16);
                f.Draw(fb, qx + (size - f.TextWidth(cap)) / 2, qy + size + 22, cap);
            }
            text_right = qx - 8;
        }
    }
    const Font& f = fonts.m26;
    const int line_h = f.ascent() + 14;
    int y = kHeader + 16 + f.ascent();
    for (const std::string& line : lines) {
        if (line.empty()) {
            y += line_h / 2;
            continue;
        }
        // Word-wrap by characters.
        const char* p = line.c_str();
        while (*p && y < kH) {
            std::string seg;
            int w = 0;
            while (*p) {
                const char* s = p;
                const int adv = f.Advance(NextCodepoint(&p));
                if (w + adv > text_right - 14) { p = s; break; }
                w += adv;
                seg.append(s, p - s);
            }
            f.Draw(fb, 14, y, seg);
            y += line_h;
        }
    }
}

}  // namespace hk
