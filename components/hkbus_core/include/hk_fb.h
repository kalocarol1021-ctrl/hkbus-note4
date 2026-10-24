// 400x300 1-bit framebuffer in the exact layout the Zectrix EPD driver wants:
// row-major, MSB first, bit 1 = white, bit 0 = black.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hk {

class Framebuffer {
public:
    static constexpr int kWidth = 400;
    static constexpr int kHeight = 300;
    static constexpr int kStride = kWidth / 8;
    static constexpr size_t kBytes = kStride * kHeight;

    void Clear(bool black = false) { std::memset(px_, black ? 0x00 : 0xFF, kBytes); }

    void Pixel(int x, int y, bool black) {
        if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
        uint8_t& b = px_[y * kStride + (x >> 3)];
        const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
        if (black) b &= static_cast<uint8_t>(~mask);
        else b |= mask;
    }

    bool Get(int x, int y) const {  // true = black
        if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return false;
        return (px_[y * kStride + (x >> 3)] & (0x80 >> (x & 7))) == 0;
    }

    void FillRect(int x, int y, int w, int h, bool black);
    void HLine(int x, int y, int w, bool black = true) { FillRect(x, y, w, 1, black); }
    void VLine(int x, int y, int h, bool black = true) { FillRect(x, y, 1, h, black); }
    void Rect(int x, int y, int w, int h, int thickness = 1, bool black = true);
    // Filled rectangle with rounded corners of radius r.
    void FillRoundRect(int x, int y, int w, int h, int r, bool black);
    // Dashed horizontal line (on/off lengths in pixels).
    void DashedHLine(int x, int y, int w, int on = 4, int off = 4);

    uint8_t* data() { return px_; }
    const uint8_t* data() const { return px_; }
    static constexpr size_t size() { return kBytes; }

private:
    uint8_t px_[kBytes];
};

}  // namespace hk
