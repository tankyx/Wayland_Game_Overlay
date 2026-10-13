// Self-check for the pure menu helpers in src/menu.h. No Wayland, no framework.
#include "overlay_menu.h"

#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

bool close(float a, float b) { return std::fabs(a - b) < 1e-5f; }
} // namespace

int main() {
    check(menuKeyFor(KEY_UP) == MenuKey::Up, "KEY_UP -> Up");
    check(menuKeyFor(KEY_DOWN) == MenuKey::Down, "KEY_DOWN -> Down");
    check(menuKeyFor(KEY_LEFT) == MenuKey::Left, "KEY_LEFT -> Left");
    check(menuKeyFor(KEY_RIGHT) == MenuKey::Right, "KEY_RIGHT -> Right");
    check(menuKeyFor(KEY_ESC) == MenuKey::Close, "KEY_ESC -> Close");
    check(menuKeyFor(KEY_ENTER) == MenuKey::Activate, "KEY_ENTER -> Activate");
    check(menuKeyFor(KEY_SPACE) == MenuKey::Activate, "KEY_SPACE -> Activate");
    check(menuKeyFor(KEY_TAB) == MenuKey::Down, "KEY_TAB -> Down");
    check(menuKeyFor(KEY_A) == MenuKey::Unknown, "KEY_A -> Unknown");

    check(close(clampStep(0.5f, 0, 1, 0.1f), 0.5f), "clamp 0.5 on step");
    check(close(clampStep(0.23f, 0, 1, 0.1f), 0.2f), "clamp snap 0.23 -> 0.2");
    check(close(clampStep(1.5f, 0, 1, 0.1f), 1.0f), "clamp upper bound");
    check(close(clampStep(-5.f, 0, 10, 1.f), 0.0f), "clamp lower bound");

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("menu self-check: all checks passed\n");
    return 0;
}
