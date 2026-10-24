// Reader/renderer for the "HKF1" 1-bit bitmap fonts produced by
// tools/gen_font.py. The font data is used in place (e.g. straight from
// flash via EMBED_FILES); nothing is copied.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "hk_fb.h"

namespace hk {

// Decode one UTF-8 code point starting at *p and advance *p.
// Invalid bytes decode as U+FFFD.
uint32_t NextCodepoint(const char** p);

class Font {
public:
    Font() = default;
    Font(const uint8_t* data, size_t size) { Load(data, size); }
    bool Load(const uint8_t* data, size_t size);
    bool ok() const { return count_ > 0; }

    int pixel_size() const { return size_; }
    int ascent() const { return ascent_; }     // pixels above baseline
    int descent() const { return descent_; }   // pixels below baseline

    // Glyphs not found here are looked up in `fallback` (if set).
    void SetFallback(const Font* fallback) { fallback_ = fallback; }

    int Advance(uint32_t cp) const;
    int TextWidth(const char* utf8) const;
    int TextWidth(const std::string& s) const { return TextWidth(s.c_str()); }

    // Draw text with its baseline at y. Returns the pen x after the text.
    // black=false draws white ink (for text on a black background).
    int Draw(Framebuffer& fb, int x, int baseline, const char* utf8,
             bool black = true) const;
    int Draw(Framebuffer& fb, int x, int baseline, const std::string& s,
             bool black = true) const {
        return Draw(fb, x, baseline, s.c_str(), black);
    }

    // Longest prefix of `utf8` that fits in max_width, with "…" appended
    // when it had to be shortened.
    std::string Ellipsize(const std::string& utf8, int max_width) const;

    bool Has(uint32_t cp) const { return Find(cp) != nullptr; }

private:
    struct Entry {
        uint32_t cp;
        uint32_t offset;
        uint8_t w, h;
        int8_t xo, yo;
        uint8_t adv;
        uint8_t pad[3];
    };
    static_assert(sizeof(Entry) == 16, "font entry layout");

    const Entry* Find(uint32_t cp) const;
    int DrawGlyph(Framebuffer& fb, int x, int baseline, uint32_t cp,
                  bool black) const;

    const uint8_t* data_ = nullptr;
    const Entry* entries_ = nullptr;
    const uint8_t* bitmaps_ = nullptr;
    uint32_t count_ = 0;
    int size_ = 0;
    int ascent_ = 0;
    int descent_ = 0;
    const Font* fallback_ = nullptr;
};

}  // namespace hk
