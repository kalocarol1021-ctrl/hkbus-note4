#include "hk_font.h"

#include <cstring>

namespace hk {

uint32_t NextCodepoint(const char** pp) {
    const auto* p = reinterpret_cast<const uint8_t*>(*pp);
    uint32_t cp = 0xFFFD;
    int extra = 0;
    if (p[0] < 0x80) {
        cp = p[0];
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07;
        extra = 3;
    } else {
        *pp += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            *pp += i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp += 1 + extra;
    return cp;
}

namespace {
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

bool Font::Load(const uint8_t* data, size_t size) {
    count_ = 0;
    if (data == nullptr || size < 16 || std::memcmp(data, "HKF1", 4) != 0) return false;
    const uint32_t count = Le32(data + 8);
    if (16 + static_cast<size_t>(count) * sizeof(Entry) > size) return false;
    data_ = data;
    size_ = Le16(data + 4);
    ascent_ = static_cast<int16_t>(Le16(data + 12));
    descent_ = static_cast<int16_t>(Le16(data + 14));
    entries_ = reinterpret_cast<const Entry*>(data + 16);
    bitmaps_ = data + 16 + count * sizeof(Entry);
    count_ = count;
    return true;
}

const Font::Entry* Font::Find(uint32_t cp) const {
    uint32_t lo = 0, hi = count_;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const uint32_t v = entries_[mid].cp;
        if (v == cp) return &entries_[mid];
        if (v < cp) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

int Font::Advance(uint32_t cp) const {
    if (const Entry* e = Find(cp)) return e->adv;
    if (fallback_ != nullptr && fallback_->Has(cp)) return fallback_->Advance(cp);
    // Missing glyph: drawn as an empty box of roughly one em.
    return size_ * 9 / 10;
}

int Font::TextWidth(const char* s) const {
    int w = 0;
    while (*s) w += Advance(NextCodepoint(&s));
    return w;
}

int Font::DrawGlyph(Framebuffer& fb, int x, int baseline, uint32_t cp,
                    bool black) const {
    const Entry* e = Find(cp);
    if (e == nullptr) {
        if (fallback_ != nullptr && fallback_->Has(cp)) {
            return fallback_->DrawGlyph(fb, x, baseline, cp, black);
        }
        const int adv = size_ * 9 / 10;
        const int box = ascent_ * 8 / 10;
        fb.Rect(x + 2, baseline - box, adv - 4, box, 1, black);
        return adv;
    }
    const uint8_t* bits = bitmaps_ + e->offset;
    const int stride = (e->w + 7) / 8;
    const int gx = x + e->xo;
    const int gy = baseline + e->yo;
    for (int row = 0; row < e->h; ++row) {
        const uint8_t* line = bits + row * stride;
        for (int col = 0; col < e->w; ++col) {
            if (line[col >> 3] & (0x80 >> (col & 7))) fb.Pixel(gx + col, gy + row, black);
        }
    }
    return e->adv;
}

int Font::Draw(Framebuffer& fb, int x, int baseline, const char* s,
               bool black) const {
    while (*s) {
        const uint32_t cp = NextCodepoint(&s);
        x += DrawGlyph(fb, x, baseline, cp, black);
    }
    return x;
}

std::string Font::Ellipsize(const std::string& text, int max_width) const {
    if (TextWidth(text) <= max_width) return text;
    const char* ell = "\xE2\x80\xA6";  // …
    const int ell_w = TextWidth(ell);
    std::string out;
    int w = 0;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        const int adv = Advance(NextCodepoint(&p));
        if (w + adv + ell_w > max_width) break;
        w += adv;
        out.append(start, p - start);
    }
    return out + ell;
}

}  // namespace hk
