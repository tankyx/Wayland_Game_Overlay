#pragma once
// Pure menu helpers shared by WaylandOverlay.cpp and the self-test. No Wayland
// dependency. Key handling uses raw evdev keycodes (no xkbcommon).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <linux/input-event-codes.h>

struct MenuItem {
    enum Type { Toggle, Slider, Button };
    Type type = Toggle;
    int id = -1; // >= 0: app-registered setting; < 0: built-in
    std::string name;
    float min = 0, max = 1, step = 1, value = 0;
};

enum class MenuKey { None, Up, Down, Left, Right, Activate, Close };

inline MenuKey menuKeyFor(uint32_t evdevKey) {
    switch (evdevKey) {
    case KEY_UP: return MenuKey::Up;
    case KEY_DOWN: return MenuKey::Down;
    case KEY_LEFT: return MenuKey::Left;
    case KEY_RIGHT: return MenuKey::Right;
    case KEY_ESC: return MenuKey::Close;
    case KEY_ENTER:
    case KEY_SPACE: return MenuKey::Activate;
    case KEY_TAB: return MenuKey::Down;
    default: return MenuKey::None;
    }
}

inline float clampStep(float v, float min, float max, float step) {
    float r = std::min(max, std::max(min, v));
    if (step > 0.0f) {
        r = min + std::roundf((r - min) / step) * step;
    }
    return std::min(max, std::max(min, r));
}
