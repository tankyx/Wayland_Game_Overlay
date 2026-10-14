#pragma once
// Pure software rasterizer helpers, shared by WaylandOverlay.cpp and the
// self-test. No Wayland dependency. Colors are 0xAARRGGBB uint32 values
// (bit 31..24 = A, 23..16 = R, 15..8 = G, 7..0 = B), matching wl_shm ARGB8888.
//
// All fills are alpha-composited (src-over). Opaque sources are a fast path
// that is pixel-identical to overwrite, so existing opaque rendering is
// unchanged; sources with A < 0xFF blend.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// src-over blend, non-premultiplied ARGB. Fast paths for the common cases.
inline uint32_t blendPixel(uint32_t dst, uint32_t src) {
    const uint32_t sa = src >> 24;
    if (sa == 0) return dst;
    if (sa == 0xFF) return src;
    const uint32_t da = dst >> 24;
    if (da == 0) return src; // over transparent: result is src
    const uint32_t inv = 0xFF - sa;
    const uint32_t outA = sa + (da * inv + 0x7F) / 0xFF;
    if (outA == 0) return 0;
    const auto ch = [&](uint32_t s, uint32_t d) {
        const uint32_t sp = (s * sa + 0x7F) / 0xFF; // premultiplied
        const uint32_t dp = (d * da + 0x7F) / 0xFF;
        const uint32_t v = sp + (dp * inv + 0x7F) / 0xFF;
        const uint32_t c = (v * 0xFF + outA / 2) / outA; // un-premultiply
        return c > 0xFF ? 0xFF : c;
    };
    return (outA << 24) | (ch((src >> 16) & 0xFF, (dst >> 16) & 0xFF) << 16) |
           (ch((src >> 8) & 0xFF, (dst >> 8) & 0xFF) << 8) | ch(src & 0xFF, dst & 0xFF);
}

inline void fillRect(uint32_t *px, int surfW, int surfH, int x, int y, int w, int h,
                     uint32_t color) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(surfW, x + w), y1 = std::min(surfH, y + h);
    for (int row = y0; row < y1; ++row) {
        uint32_t *line = px + static_cast<size_t>(row) * surfW;
        for (int col = x0; col < x1; ++col) line[col] = blendPixel(line[col], color);
    }
}

// Thick line via a square brush swept along the segment (software, no AA).
inline void drawLine(uint32_t *px, int surfW, int surfH, float x0, float y0, float x1,
                     float y1, float thickness, uint32_t color) {
    const int half = std::max(1, static_cast<int>(thickness * 0.5f));
    const float dx = x1 - x0, dy = y1 - y0;
    const int steps = static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
    if (steps <= 0) {
        fillRect(px, surfW, surfH, static_cast<int>(std::lround(x0)) - half,
                 static_cast<int>(std::lround(y0)) - half, half * 2, half * 2, color);
        return;
    }
    for (int s = 0; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);
        const int x = static_cast<int>(std::lround(x0 + dx * t)) - half;
        const int y = static_cast<int>(std::lround(y0 + dy * t)) - half;
        fillRect(px, surfW, surfH, x, y, half * 2, half * 2, color);
    }
}

inline uint32_t lerpColor(uint32_t a, uint32_t b, float t) {
    const auto lerp8 = [](uint32_t x, uint32_t y, float f) -> uint32_t {
        return static_cast<uint32_t>(
            std::lroundf(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * f));
    };
    return (lerp8((a >> 24) & 0xFF, (b >> 24) & 0xFF, t) << 24) |
           (lerp8((a >> 16) & 0xFF, (b >> 16) & 0xFF, t) << 16) |
           (lerp8((a >> 8) & 0xFF, (b >> 8) & 0xFF, t) << 8) |
           lerp8(a & 0xFF, b & 0xFF, t);
}

// Linear gradient fill over a rect. axis: 0 = horizontal (left->right),
// 1 = vertical (top->bottom). Endpoints hit c0/c1 exactly.
inline void fillRectGradient(uint32_t *px, int surfW, int surfH, int x, int y,
                             int w, int h, uint32_t c0, uint32_t c1, int axis) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(surfW, x + w), y1 = std::min(surfH, y + h);
    if (x0 >= x1 || y0 >= y1) return;
    const float denomX = static_cast<float>(std::max(1, x1 - x0 - 1));
    const float denomY = static_cast<float>(std::max(1, y1 - y0 - 1));
    for (int row = y0; row < y1; ++row) {
        uint32_t *line = px + static_cast<size_t>(row) * surfW;
        const float ty = static_cast<float>(row - y0) / denomY;
        for (int col = x0; col < x1; ++col) {
            const float t = axis == 1 ? ty : static_cast<float>(col - x0) / denomX;
            line[col] = blendPixel(line[col], lerpColor(c0, c1, t));
        }
    }
}

inline void fillEllipse(uint32_t *px, int surfW, int surfH, float cx, float cy,
                        float rx, float ry, uint32_t color) {
    const float rxx = std::max(1.0f, rx), ryy = std::max(1.0f, ry);
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - rxx)));
    const int x1 = std::min(surfW, static_cast<int>(std::ceil(cx + rxx)) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - ryy)));
    const int y1 = std::min(surfH, static_cast<int>(std::ceil(cy + ryy)) + 1);
    for (int row = y0; row < y1; ++row) {
        uint32_t *line = px + static_cast<size_t>(row) * surfW;
        const float dy = (static_cast<float>(row) - cy) / ryy;
        for (int col = x0; col < x1; ++col) {
            const float dx = (static_cast<float>(col) - cx) / rxx;
            if (dx * dx + dy * dy <= 1.0f) line[col] = blendPixel(line[col], color);
        }
    }
}

inline void fillEllipseGradient(uint32_t *px, int surfW, int surfH, float cx,
                                float cy, float rx, float ry, uint32_t c0,
                                uint32_t c1, int axis) {
    const float rxx = std::max(1.0f, rx), ryy = std::max(1.0f, ry);
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - rxx)));
    const int x1 = std::min(surfW, static_cast<int>(std::ceil(cx + rxx)) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - ryy)));
    const int y1 = std::min(surfH, static_cast<int>(std::ceil(cy + ryy)) + 1);
    const float denomX = static_cast<float>(std::max(1, x1 - x0 - 1));
    const float denomY = static_cast<float>(std::max(1, y1 - y0 - 1));
    for (int row = y0; row < y1; ++row) {
        uint32_t *line = px + static_cast<size_t>(row) * surfW;
        const float dy = (static_cast<float>(row) - cy) / ryy;
        const float ty = static_cast<float>(row - y0) / denomY;
        for (int col = x0; col < x1; ++col) {
            const float dx = (static_cast<float>(col) - cx) / rxx;
            if (dx * dx + dy * dy <= 1.0f) {
                const float t = axis == 1 ? ty : static_cast<float>(col - x0) / denomX;
                line[col] = blendPixel(line[col], lerpColor(c0, c1, t));
            }
        }
    }
}

// Filled polygon, even-odd rule. pts is x,y pairs (2N floats). Fewer than 3
// points is a no-op.
inline void fillPolygon(uint32_t *px, int surfW, int surfH,
                        const std::vector<float> &pts, uint32_t color) {
    const size_t n = pts.size() / 2;
    if (n < 3) return;
    float minY = pts[1], maxY = pts[1];
    for (size_t i = 0; i < n; ++i) {
        minY = std::min(minY, pts[i * 2 + 1]);
        maxY = std::max(maxY, pts[i * 2 + 1]);
    }
    const int y0 = std::max(0, static_cast<int>(std::floor(minY)));
    const int y1 = std::min(surfH, static_cast<int>(std::ceil(maxY)) + 1);
    std::vector<float> xs;
    xs.reserve(n);
    for (int row = y0; row < y1; ++row) {
        const float y = static_cast<float>(row) + 0.5f; // pixel center
        xs.clear();
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            const float y0p = pts[i * 2 + 1], y1p = pts[j * 2 + 1];
            if ((y0p <= y && y1p > y) || (y1p <= y && y0p > y)) {
                const float x0p = pts[i * 2], x1p = pts[j * 2];
                xs.push_back(x0p + (y - y0p) / (y1p - y0p) * (x1p - x0p));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int sx = std::max(0, static_cast<int>(std::ceil(xs[k])));
            const int ex = std::min(surfW, static_cast<int>(std::ceil(xs[k + 1])));
            uint32_t *line = px + static_cast<size_t>(row) * surfW;
            for (int col = sx; col < ex; ++col) line[col] = blendPixel(line[col], color);
        }
    }
}

// 4-sided rect border, thickness px wide.
inline void fillRectOutline(uint32_t *px, int surfW, int surfH, int x, int y, int w,
                            int h, int thickness, uint32_t color) {
    const int t = std::max(1, thickness);
    fillRect(px, surfW, surfH, x, y, w, t, color);              // top
    fillRect(px, surfW, surfH, x, y + h - t, w, t, color);      // bottom
    fillRect(px, surfW, surfH, x, y + t, t, h - 2 * t, color);  // left
    fillRect(px, surfW, surfH, x + w - t, y + t, t, h - 2 * t, color); // right
}

inline void fillEllipseOutline(uint32_t *px, int surfW, int surfH, float cx, float cy,
                               float rx, float ry, float thickness, uint32_t color);

// Filled rounded rect (9-patch: cross + four corner discs).
inline void fillRoundedRect(uint32_t *px, int surfW, int surfH, int x, int y, int w,
                            int h, float radius, uint32_t color) {
    const int cr = std::max(0, static_cast<int>(radius));
    if (cr <= 0 || w <= 0 || h <= 0) {
        fillRect(px, surfW, surfH, x, y, w, h, color);
        return;
    }
    const int r = std::min(cr, std::min(w, h) / 2);
    fillRect(px, surfW, surfH, x + r, y, w - 2 * r, h, color);     // horizontal
    fillRect(px, surfW, surfH, x, y + r, w, h - 2 * r, color);     // vertical
    fillEllipse(px, surfW, surfH, x + r, y + r, r, r, color);
    fillEllipse(px, surfW, surfH, x + w - r, y + r, r, r, color);
    fillEllipse(px, surfW, surfH, x + r, y + h - r, r, r, color);
    fillEllipse(px, surfW, surfH, x + w - r, y + h - r, r, r, color);
}

// Rounded rect border: four straight edges + four corner rings.
inline void fillRoundedRectOutline(uint32_t *px, int surfW, int surfH, int x, int y,
                                   int w, int h, float radius, int thickness,
                                   uint32_t color) {
    const int cr = std::max(0, static_cast<int>(radius));
    if (cr <= 0) {
        fillRectOutline(px, surfW, surfH, x, y, w, h, thickness, color);
        return;
    }
    const int r = std::min(cr, std::min(w, h) / 2);
    const int t = std::max(1, thickness);
    fillRect(px, surfW, surfH, x + r, y, w - 2 * r, t, color);            // top
    fillRect(px, surfW, surfH, x + r, y + h - t, w - 2 * r, t, color);    // bottom
    fillRect(px, surfW, surfH, x, y + r, t, h - 2 * r, color);            // left
    fillRect(px, surfW, surfH, x + w - t, y + r, t, h - 2 * r, color);    // right
    fillEllipseOutline(px, surfW, surfH, x + r, y + r, r, r, static_cast<float>(t), color);
    fillEllipseOutline(px, surfW, surfH, x + w - r, y + r, r, r, static_cast<float>(t), color);
    fillEllipseOutline(px, surfW, surfH, x + r, y + h - r, r, r, static_cast<float>(t), color);
    fillEllipseOutline(px, surfW, surfH, x + w - r, y + h - r, r, r, static_cast<float>(t), color);
}

// Ellipse ring: pixels inside (rx,ry) but outside (rx-t, ry-t).
inline void fillEllipseOutline(uint32_t *px, int surfW, int surfH, float cx, float cy,
                               float rx, float ry, float thickness, uint32_t color) {
    const float rxx = std::max(1.0f, rx), ryy = std::max(1.0f, ry);
    const float t = std::max(1.0f, thickness);
    const float irx = std::max(1.0f, rxx - t), iry = std::max(1.0f, ryy - t);
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - rxx)));
    const int x1 = std::min(surfW, static_cast<int>(std::ceil(cx + rxx)) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - ryy)));
    const int y1 = std::min(surfH, static_cast<int>(std::ceil(cy + ryy)) + 1);
    for (int row = y0; row < y1; ++row) {
        uint32_t *line = px + static_cast<size_t>(row) * surfW;
        const float dy = (static_cast<float>(row) - cy) / ryy;
        const float idy = (static_cast<float>(row) - cy) / iry;
        for (int col = x0; col < x1; ++col) {
            const float dx = (static_cast<float>(col) - cx) / rxx;
            const float idx = (static_cast<float>(col) - cx) / irx;
            const float d2 = dx * dx + dy * dy;
            const float i2 = idx * idx + idy * idy;
            if (d2 <= 1.0f && i2 >= 1.0f) line[col] = blendPixel(line[col], color);
        }
    }
}

// Closed thick polyline (one drawLine per edge).
inline void strokePolygon(uint32_t *px, int surfW, int surfH, const std::vector<float> &pts,
                          float thickness, uint32_t color) {
    const size_t n = pts.size() / 2;
    if (n < 3) return;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        drawLine(px, surfW, surfH, pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1],
                 thickness, color);
    }
}

// ---------------------------------------------------------------------------
// Anti-aliased variants (1 px edge feather via signed distance / supersampling)
// ---------------------------------------------------------------------------

inline float segDist(float px, float py, float ax, float ay, float bx, float by) {
    const float abx = bx - ax, aby = by - ay;
    const float apx = px - ax, apy = py - ay;
    const float denom = abx * abx + aby * aby;
    float t = denom > 1e-9f ? (apx * abx + apy * aby) / denom : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float cx = ax + abx * t, cy = ay + aby * t;
    const float dx = px - cx, dy = py - cy;
    return std::sqrt(dx * dx + dy * dy);
}

// Coverage in [0,255] from a signed distance d (positive outside), 1 px feather.
inline uint32_t coverageAlpha(float d) {
    const float cov = 0.5f - d;
    if (cov <= 0.0f) return 0;
    if (cov >= 1.0f) return 255;
    return static_cast<uint32_t>(cov * 255.0f);
}

inline void blendPixelCov(uint32_t *px, int surfW, int surfH, int x, int y,
                          uint32_t color, uint32_t cov) {
    if (x < 0 || y < 0 || x >= surfW || y >= surfH || cov == 0) return;
    const uint32_t sa = (color >> 24) * cov / 255;
    if (sa == 0) return;
    const uint32_t c = (color & 0x00FFFFFF) | (sa << 24);
    px[static_cast<size_t>(y) * surfW + x] =
        blendPixel(px[static_cast<size_t>(y) * surfW + x], c);
}

inline void drawLineAA(uint32_t *px, int surfW, int surfH, float x0, float y0,
                       float x1, float y1, float thickness, uint32_t color) {
    const float r = std::max(0.5f, thickness * 0.5f);
    const int bx0 = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - r - 1.0f)));
    const int bx1 = std::min(surfW, static_cast<int>(std::ceil(std::max(x0, x1) + r + 1.0f)));
    const int by0 = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - r - 1.0f)));
    const int by1 = std::min(surfH, static_cast<int>(std::ceil(std::max(y0, y1) + r + 1.0f)));
    for (int row = by0; row < by1; ++row) {
        for (int col = bx0; col < bx1; ++col) {
            const float d = segDist(static_cast<float>(col) + 0.5f, static_cast<float>(row) + 0.5f,
                                    x0, y0, x1, y1) - r;
            blendPixelCov(px, surfW, surfH, col, row, color, coverageAlpha(d));
        }
    }
}

inline void fillEllipseAA(uint32_t *px, int surfW, int surfH, float cx, float cy,
                          float rx, float ry, uint32_t color) {
    const float rxx = std::max(1.0f, rx), ryy = std::max(1.0f, ry);
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - rxx - 1.0f)));
    const int x1 = std::min(surfW, static_cast<int>(std::ceil(cx + rxx + 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - ryy - 1.0f)));
    const int y1 = std::min(surfH, static_cast<int>(std::ceil(cy + ryy + 1.0f)));
    const float scale = std::min(rxx, ryy);
    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            const float dx = (static_cast<float>(col) - cx) / rxx;
            const float dy = (static_cast<float>(row) - cy) / ryy;
            const float d2 = dx * dx + dy * dy;
            const float d = (std::sqrt(d2) - 1.0f) * scale; // signed px, -ve inside
            blendPixelCov(px, surfW, surfH, col, row, color, coverageAlpha(d));
        }
    }
}

inline bool pointInPoly(const std::vector<float> &pts, float x, float y) {
    const size_t n = pts.size() / 2;
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = pts[i * 2], yi = pts[i * 2 + 1];
        const float xj = pts[j * 2], yj = pts[j * 2 + 1];
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

inline void fillPolygonAA(uint32_t *px, int surfW, int surfH,
                          const std::vector<float> &pts, uint32_t color) {
    const size_t n = pts.size() / 2;
    if (n < 3) return;
    float minX = pts[0], maxX = pts[0], minY = pts[1], maxY = pts[1];
    for (size_t i = 1; i < n; ++i) {
        minX = std::min(minX, pts[i * 2]);
        maxX = std::max(maxX, pts[i * 2]);
        minY = std::min(minY, pts[i * 2 + 1]);
        maxY = std::max(maxY, pts[i * 2 + 1]);
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(minX)));
    const int x1 = std::min(surfW, static_cast<int>(std::ceil(maxX)) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(minY)));
    const int y1 = std::min(surfH, static_cast<int>(std::ceil(maxY)) + 1);
    const float offs[2] = {0.25f, 0.75f};
    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            int hits = 0;
            for (float oy : offs) {
                for (float ox : offs) {
                    if (pointInPoly(pts, static_cast<float>(col) + ox,
                                    static_cast<float>(row) + oy)) {
                        ++hits;
                    }
                }
            }
            if (hits > 0) blendPixelCov(px, surfW, surfH, col, row, color, hits * 255 / 4);
        }
    }
}

inline void strokePolygonAA(uint32_t *px, int surfW, int surfH,
                            const std::vector<float> &pts, float thickness,
                            uint32_t color) {
    const size_t n = pts.size() / 2;
    if (n < 3) return;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        drawLineAA(px, surfW, surfH, pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1],
                   thickness, color);
    }
}
