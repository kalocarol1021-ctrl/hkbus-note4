#include "hk_fb.h"

#include <algorithm>

namespace hk {

void Framebuffer::FillRect(int x, int y, int w, int h, bool black) {
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(kWidth, x + w), y1 = std::min(kHeight, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) Pixel(xx, yy, black);
    }
}

void Framebuffer::Rect(int x, int y, int w, int h, int t, bool black) {
    FillRect(x, y, w, t, black);
    FillRect(x, y + h - t, w, t, black);
    FillRect(x, y, t, h, black);
    FillRect(x + w - t, y, t, h, black);
}

void Framebuffer::FillRoundRect(int x, int y, int w, int h, int r, bool black) {
    r = std::min(r, std::min(w, h) / 2);
    for (int yy = 0; yy < h; ++yy) {
        int inset = 0;
        int dy = -1;
        if (yy < r) dy = r - yy;
        else if (yy >= h - r) dy = yy - (h - r - 1);
        if (dy > 0) {
            // Largest dx with dx^2 + dy^2 <= r^2, measured from the corner centre.
            int dx = 0;
            while ((dx + 1) * (dx + 1) + (dy - 0) * (dy - 0) <= r * r) ++dx;
            inset = r - dx;
        }
        FillRect(x + inset, y + yy, w - 2 * inset, 1, black);
    }
}

void Framebuffer::DashedHLine(int x, int y, int w, int on, int off) {
    for (int i = 0; i < w; i += on + off) {
        FillRect(x + i, y, std::min(on, w - i), 1, true);
    }
}

}  // namespace hk
