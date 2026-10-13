// Self-check for the pure rasterizer helpers in src/raster.h. No Wayland, no
// framework: a failing assert prints and returns nonzero.
#include "raster.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr int kW = 32, kH = 32;

std::vector<uint32_t> fresh() { return std::vector<uint32_t>(kW * kH, 0u); }

uint32_t at(const std::vector<uint32_t> &b, int x, int y) { return b[y * kW + x]; }
} // namespace

int main() {
    // 1. lerp endpoints are exact.
    check(lerpColor(0xFF000000, 0xFFFFFFFF, 0.0f) == 0xFF000000, "lerp t=0");
    check(lerpColor(0xFF000000, 0xFFFFFFFF, 1.0f) == 0xFFFFFFFF, "lerp t=1");

    // 2. Horizontal gradient: leftmost == c0, rightmost == c1, middle mixes.
    {
        auto b = fresh();
        fillRectGradient(b.data(), kW, kH, 0, 0, 32, 32, 0xFFFF0000, 0xFF0000FF, 0);
        check(at(b, 0, 16) == 0xFFFF0000, "hgrad left");
        check(at(b, 31, 16) == 0xFF0000FF, "hgrad right");
        const uint32_t mid = at(b, 16, 16);
        check(((mid >> 16) & 0xFF) > 0 && (mid & 0xFF) > 0, "hgrad middle mixes");
    }

    // 3. c0 == c1 -> solid.
    {
        auto b = fresh();
        fillRectGradient(b.data(), kW, kH, 0, 0, 32, 32, 0xDEADBEEF, 0xDEADBEEF, 0);
        check(at(b, 16, 16) == 0xDEADBEEF, "gradient solid when c0==c1");
    }

    // 4. Ellipse: center set, far corner clear; zero radius clamps to 1px.
    {
        auto b = fresh();
        fillEllipse(b.data(), kW, kH, 16, 16, 8, 8, 0xFF00FF00);
        check(at(b, 16, 16) == 0xFF00FF00, "ellipse center");
        check(at(b, 1, 1) == 0, "ellipse corner clear");
    }
    {
        auto b = fresh();
        fillEllipse(b.data(), kW, kH, 16, 16, 0, 0, 0xFF00FF00);
        check(at(b, 16, 16) == 0xFF00FF00, "ellipse zero radius clamps");
    }

    // 5. Triangle: inside set, outside clear; <3 points ignored.
    {
        auto b = fresh();
        fillPolygon(b.data(), kW, kH, {0.f, 0.f, 31.f, 0.f, 15.f, 31.f}, 0xFF00FFFF);
        check(at(b, 15, 5) == 0xFF00FFFF, "triangle inside");
        check(at(b, 30, 30) == 0, "triangle outside clear");
    }
    {
        auto b = fresh();
        fillPolygon(b.data(), kW, kH, {0.f, 0.f, 31.f, 0.f}, 0xFF00FFFF);
        bool any = false;
        for (uint32_t p : b) any |= (p != 0);
        check(!any, "2-point polygon ignored");
    }

    // 6. Ellipse outline: ring (center empty, boundary filled).
    {
        auto b = fresh();
        fillEllipseOutline(b.data(), kW, kH, 16, 16, 8, 8, 2.0f, 0xFF00FF00);
        check(at(b, 16, 16) == 0, "ellipse outline center empty");
        check(at(b, 16, 8) == 0xFF00FF00, "ellipse outline boundary filled");
    }

    // 7. Rect outline: border filled, interior empty.
    {
        auto b = fresh();
        fillRectOutline(b.data(), kW, kH, 0, 0, 32, 32, 2, 0xFF00FF00);
        check(at(b, 0, 0) == 0xFF00FF00, "rect outline corner");
        check(at(b, 0, 16) == 0xFF00FF00, "rect outline left edge");
        check(at(b, 16, 16) == 0, "rect outline interior empty");
    }

    // 8. Polygon outline (triangle): edge filled, interior empty.
    {
        auto b = fresh();
        strokePolygon(b.data(), kW, kH, {0.f, 0.f, 31.f, 0.f, 15.f, 31.f}, 2.0f, 0xFF00FFFF);
        check(at(b, 15, 0) == 0xFF00FFFF, "polygon outline top edge");
        check(at(b, 15, 10) == 0, "polygon outline interior empty");
    }

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("raster self-check: all checks passed\n");
    return 0;
}
