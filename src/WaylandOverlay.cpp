#ifndef _GNU_SOURCE
#define _GNU_SOURCE // memfd_create, eventfd
#endif

#include "WaylandOverlay.h"

#include <wayland-client.h>
#include "wayland/xdg-shell-client-protocol.h"
#include "wayland/wlr-layer-shell-unstable-v1-client-protocol.h"
#include "raster.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

// ---------------------------------------------------------------------------
// Built-in 5x7 bitmap font (classic public-domain "glcdfont" glyphs).
// Each glyph is 5 columns; bit 0 of each column byte is the TOP row.
// Lowercase letters reuse the uppercase glyphs.
// ---------------------------------------------------------------------------
namespace {

struct Glyph {
    char c;
    uint8_t col[5];
};

// clang-format off
static const Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'_', {0x40, 0x40, 0x40, 0x40, 0x40}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {'|', {0x00, 0x00, 0x7F, 0x00, 0x00}},
    {'(', {0x00, 0x1C, 0x22, 0x41, 0x00}},
    {')', {0x00, 0x41, 0x22, 0x1C, 0x00}},
    {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
};
// clang-format on

const Glyph *findGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const auto &g : kFont) {
        if (g.c == c) return &g;
    }
    static const Glyph fallback{'?', {0x02, 0x01, 0x51, 0x09, 0x06}};
    return &fallback;
}

// Opaque per-label colors (0xAARRGGBB; wl_shm ARGB8888 on little-endian).
static const uint32_t kPalette[] = {
    0xFFFF4444, // red
    0xFF44DD44, // green
    0xFF4499FF, // blue
    0xFFFFDD33, // yellow
    0xFFFF44FF, // magenta
    0xFF33DDDD, // cyan
    0xFFFF9933, // orange
    0xFFAA66FF, // purple
};
static constexpr size_t kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

uint32_t labelColor(int label) {
    const size_t idx = static_cast<size_t>(label < 0 ? 0 : label) % kPaletteSize;
    return kPalette[idx];
}

} // namespace

// ---------------------------------------------------------------------------
// Wayland callbacks
// ---------------------------------------------------------------------------

void WaylandOverlay::registryGlobal(void *data, wl_registry *reg, uint32_t name,
                                  const char *iface, uint32_t version) {
    auto *self = static_cast<WaylandOverlay *>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        self->_compositor = static_cast<wl_compositor *>(
            wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        self->_shm = static_cast<wl_shm *>(
            wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        self->_layerShell = static_cast<zwlr_layer_shell_v1 *>(
            wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u)));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        // v4 gives the connector name (wl_output.name).
        auto *op = static_cast<wl_output *>(
            wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 4u)));
        auto *info = new OutputInfo();
        info->output = op;
        static const wl_output_listener outputListener = {
            &WaylandOverlay::outputGeometry, &WaylandOverlay::outputMode,
            &WaylandOverlay::outputDone,     &WaylandOverlay::outputScale,
            &WaylandOverlay::outputName,     &WaylandOverlay::outputDescription,
        };
        wl_output_add_listener(op, &outputListener, info);
        self->_outputs.push_back(info);
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        self->_seat = static_cast<wl_seat *>(
            wl_registry_bind(reg, name, &wl_seat_interface, std::min(version, 7u)));
        static const wl_seat_listener seatListener = {
            &WaylandOverlay::seatCapabilities,
            &WaylandOverlay::seatName,
        };
        wl_seat_add_listener(self->_seat, &seatListener, self);
    }
}

void WaylandOverlay::seatCapabilities(void *data, wl_seat *seat, uint32_t caps) {
    auto *self = static_cast<WaylandOverlay *>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->_pointer) {
        self->_pointer = wl_seat_get_pointer(seat);
        static const wl_pointer_listener pointerListener = {
            &WaylandOverlay::pointerEnter,
            &WaylandOverlay::pointerLeave,
            &WaylandOverlay::pointerMotion,
            &WaylandOverlay::pointerButton,
            &WaylandOverlay::pointerAxis,
            &WaylandOverlay::pointerFrame,
            &WaylandOverlay::pointerAxisSource,
            &WaylandOverlay::pointerAxisStop,
            &WaylandOverlay::pointerAxisDiscrete,
            &WaylandOverlay::pointerAxisValue120,
            &WaylandOverlay::pointerAxisRelativeDirection,
            &WaylandOverlay::pointerWarp,
        };
        wl_pointer_add_listener(self->_pointer, &pointerListener, self);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->_keyboard) {
        self->_keyboard = wl_seat_get_keyboard(seat);
        static const wl_keyboard_listener keyboardListener = {
            &WaylandOverlay::keyboardKeymap,
            &WaylandOverlay::keyboardEnter,
            &WaylandOverlay::keyboardLeave,
            &WaylandOverlay::keyboardKey,
            &WaylandOverlay::keyboardModifiers,
            &WaylandOverlay::keyboardRepeatInfo,
        };
        wl_keyboard_add_listener(self->_keyboard, &keyboardListener, self);
    }
}

void WaylandOverlay::pointerEnter(void *data, wl_pointer *, uint32_t, wl_surface *,
                                  int32_t x, int32_t y) {
    auto *self = static_cast<WaylandOverlay *>(data);
    self->_ptrX = x;
    self->_ptrY = y;
}

void WaylandOverlay::pointerLeave(void *data, wl_pointer *, uint32_t, wl_surface *) {
    (void)data;
}

void WaylandOverlay::pointerMotion(void *data, wl_pointer *, uint32_t, int32_t x, int32_t y) {
    auto *self = static_cast<WaylandOverlay *>(data);
    self->_ptrX = x;
    self->_ptrY = y;
}

void WaylandOverlay::pointerButton(void *data, wl_pointer *, uint32_t, uint32_t, uint32_t,
                                   uint32_t) {
    (void)data; // interaction lands in Task 5
}

void WaylandOverlay::keyboardKey(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t,
                                 uint32_t) {
    (void)data; // interaction lands in Task 5
}

void WaylandOverlay::outputName(void *data, wl_output *, const char *name) {
    static_cast<OutputInfo *>(data)->name = name ? name : "";
}

void WaylandOverlay::outputMode(void *data, wl_output *, uint32_t flags, int32_t w, int32_t h, int32_t) {
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        auto *info = static_cast<OutputInfo *>(data);
        info->width = w;
        info->height = h;
    }
}

void WaylandOverlay::layerConfigure(void *data, zwlr_layer_surface_v1 *ls,
                                  uint32_t serial, uint32_t w, uint32_t h) {
    auto *self = static_cast<WaylandOverlay *>(data);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (w > 0 && h > 0 &&
        (static_cast<int>(w) != self->_width || static_cast<int>(h) != self->_height)) {
        self->allocBuffers(static_cast<int>(w), static_cast<int>(h));
    }
    self->_configured = true;
}

void WaylandOverlay::layerClosed(void *data, zwlr_layer_surface_v1 *) {
    static_cast<WaylandOverlay *>(data)->_running.store(false);
}

void WaylandOverlay::bufferRelease(void *data, wl_buffer *) {
    static_cast<Buffer *>(data)->busy = false;
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

bool WaylandOverlay::initBuffer(Buffer &b, int w, int h) {
    const size_t stride = static_cast<size_t>(w) * 4;
    const size_t size = stride * static_cast<size_t>(h);

    int fd = memfd_create("wayland-overlay", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
        std::cerr << "[WaylandOverlay] Failed to allocate shm buffer: "
                  << std::strerror(errno) << std::endl;
        if (fd >= 0) close(fd);
        return false;
    }

    void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        std::cerr << "[WaylandOverlay] mmap failed: " << std::strerror(errno) << std::endl;
        close(fd);
        return false;
    }

    wl_shm_pool *pool = wl_shm_create_pool(_shm, fd, static_cast<int32_t>(size));
    b.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, static_cast<int32_t>(stride),
                                         WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    static const wl_buffer_listener bufferListener = {&WaylandOverlay::bufferRelease};
    wl_buffer_add_listener(b.buffer, &bufferListener, &b);

    b.data = static_cast<uint32_t *>(mem);
    b.size = size;
    b.busy = false;
    b.hasPaint = false;
    return true;
}

void WaylandOverlay::destroyBuffer(Buffer &b) {
    if (b.buffer) {
        wl_buffer_destroy(b.buffer);
        b.buffer = nullptr;
    }
    if (b.data) {
        munmap(b.data, b.size);
        b.data = nullptr;
    }
    b.size = 0;
    b.busy = false;
    b.hasPaint = false;
}

void WaylandOverlay::allocBuffers(int w, int h) {
    std::cout << "[WaylandOverlay] Layer surface configured: " << w << "x" << h << std::endl;
    for (auto &b : _buffers) destroyBuffer(b);
    _lastAttached = nullptr;
    _width = w;
    _height = h;
    for (auto &b : _buffers) initBuffer(b, w, h);
}

// ---------------------------------------------------------------------------
// Setup / teardown (render thread)
// ---------------------------------------------------------------------------

wl_output *WaylandOverlay::pickOutput() const {
    const char *envName = std::getenv("WAYLAND_OVERLAY_OUTPUT");
    if (envName && *envName) {
        for (auto *o : _outputs) {
            if (o->name == envName) return o->output;
        }
        std::cerr << "[WaylandOverlay] WAYLAND_OVERLAY_OUTPUT='" << envName
                  << "' not found; using default output" << std::endl;
    }
    // Prefer the output whose mode matches the captured screen size — that is
    // the monitor the game/capture runs on.
    if (_preferredW > 0 && _preferredH > 0) {
        for (auto *o : _outputs) {
            if (o->width == _preferredW && o->height == _preferredH) {
                std::cout << "[WaylandOverlay] Using output '" << o->name << "' (matches capture "
                          << _preferredW << "x" << _preferredH << ")" << std::endl;
                return o->output;
            }
        }
    }
    // Otherwise fall back to the largest output rather than letting the
    // compositor pick the (possibly wrong) focused one.
    wl_output *best = nullptr;
    long bestArea = -1;
    for (auto *o : _outputs) {
        const long area = static_cast<long>(o->width) * o->height;
        if (area > bestArea) {
            bestArea = area;
            best = o->output;
        }
    }
    if (best) {
        std::cout << "[WaylandOverlay] Using largest output (no exact capture-size match)" << std::endl;
    }
    return best; // nullptr lets the compositor decide (typically the focused output).
}

bool WaylandOverlay::initWayland() {
    _display = wl_display_connect(nullptr);
    if (!_display) {
        std::cerr << "[WaylandOverlay] Failed to connect to Wayland display" << std::endl;
        return false;
    }

    _registry = wl_display_get_registry(_display);
    static const wl_registry_listener registryListener = {
        &WaylandOverlay::registryGlobal,
        &WaylandOverlay::registryGlobalRemove,
    };
    wl_registry_add_listener(_registry, &registryListener, this);
    wl_display_roundtrip(_display); // bind globals
    wl_display_roundtrip(_display); // receive wl_output names

    if (!_compositor || !_shm) {
        std::cerr << "[WaylandOverlay] Missing wl_compositor / wl_shm" << std::endl;
        return false;
    }
    if (!_layerShell) {
        std::cerr << "[WaylandOverlay] Compositor does not support wlr-layer-shell; "
                     "cannot create overlay" << std::endl;
        return false;
    }

    _surface = wl_compositor_create_surface(_compositor);
    // Detections are published in output pixel coordinates; pin buffer scale
    // to 1 so buffer pixels map 1:1 onto the output (requires the compositor
    // to not use fractional scaling on this output).
    wl_surface_set_buffer_scale(_surface, 1);

    // Empty input region => fully click-through.
    wl_region *region = wl_compositor_create_region(_compositor);
    wl_surface_set_input_region(_surface, region);
    wl_region_destroy(region);

    _layerSurface = zwlr_layer_shell_v1_get_layer_surface(
        _layerShell, _surface, pickOutput(), ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wayland-overlay");
    if (!_layerSurface) {
        std::cerr << "[WaylandOverlay] Failed to create layer surface" << std::endl;
        return false;
    }

    // Anchor to all edges so the surface fills the output; negative exclusive
    // zone lets us render over panels and fullscreen windows.
    zwlr_layer_surface_v1_set_anchor(_layerSurface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(_layerSurface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(_layerSurface, 0);
    zwlr_layer_surface_v1_set_size(_layerSurface, 0, 0);

    static const zwlr_layer_surface_v1_listener layerListener = {
        &WaylandOverlay::layerConfigure,
        &WaylandOverlay::layerClosed,
    };
    zwlr_layer_surface_v1_add_listener(_layerSurface, &layerListener, this);

    // Initial commit, then roundtrip so layerConfigure fires and allocates
    // the buffers at the compositor-provided output size.
    wl_surface_commit(_surface);
    wl_display_roundtrip(_display);

    if (!_configured) {
        std::cerr << "[WaylandOverlay] Layer surface was not configured" << std::endl;
    }
    return _configured;
}

void WaylandOverlay::teardown() {
    for (auto &b : _buffers) destroyBuffer(b);
    if (_layerSurface) {
        zwlr_layer_surface_v1_destroy(_layerSurface);
        _layerSurface = nullptr;
    }
    if (_surface) {
        wl_surface_destroy(_surface);
        _surface = nullptr;
    }
    for (auto *o : _outputs) {
        if (o->output) wl_output_destroy(o->output);
        delete o;
    }
    _outputs.clear();
    if (_layerShell) {
        zwlr_layer_shell_v1_destroy(_layerShell);
        _layerShell = nullptr;
    }
    if (_shm) {
        wl_shm_destroy(_shm);
        _shm = nullptr;
    }
    if (_compositor) {
        wl_compositor_destroy(_compositor);
        _compositor = nullptr;
    }
    if (_keyboard) {
        wl_keyboard_destroy(_keyboard);
        _keyboard = nullptr;
    }
    if (_pointer) {
        wl_pointer_destroy(_pointer);
        _pointer = nullptr;
    }
    if (_seat) {
        wl_seat_destroy(_seat);
        _seat = nullptr;
    }
    if (_registry) {
        wl_registry_destroy(_registry);
        _registry = nullptr;
    }
    if (_display) {
        wl_display_flush(_display);
        wl_display_disconnect(_display);
        _display = nullptr;
    }
    _configured = false;
    _width = _height = 0;
}

// ---------------------------------------------------------------------------
// Software rendering into a wl_shm buffer
// ---------------------------------------------------------------------------

namespace {

// Half-open int rect.
struct IntRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    void add(const IntRect &o) {
        if (o.empty()) return;
        if (empty()) {
            *this = o;
            return;
        }
        x0 = std::min(x0, o.x0);
        y0 = std::min(y0, o.y0);
        x1 = std::max(x1, o.x1);
        y1 = std::max(y1, o.y1);
    }
    void clamp(int w, int h) {
        x0 = std::max(0, std::min(x0, w));
        x1 = std::max(0, std::min(x1, w));
        y0 = std::max(0, std::min(y0, h));
        y1 = std::max(0, std::min(y1, h));
    }
};

constexpr int kTextScale = 2;                       // 5x7 -> 10x14 px
constexpr int kTextAdvance = 6 * kTextScale;        // 5 glyph cols + 1 space
constexpr int kTextHeight = 7 * kTextScale;

void fillRect(uint32_t *px, int surfW, int surfH, int x, int y, int w, int h,
              uint32_t color) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(surfW, x + w), y1 = std::min(surfH, y + h);
    for (int row = y0; row < y1; ++row) {
        uint32_t *line = px + static_cast<size_t>(row) * surfW;
        for (int col = x0; col < x1; ++col) line[col] = color;
    }
}

void clearRect(uint32_t *px, int surfW, const IntRect &r) {
    if (r.empty()) return;
    const size_t words = static_cast<size_t>(r.x1 - r.x0);
    for (int row = r.y0; row < r.y1; ++row) {
        std::memset(px + static_cast<size_t>(row) * surfW + r.x0, 0, words * 4);
    }
}

void drawBox(uint32_t *px, int surfW, int surfH, float x, float y, float w, float h,
             uint32_t color) {
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    const int iw = static_cast<int>(std::lround(w));
    const int ih = static_cast<int>(std::lround(h));
    constexpr int t = 2; // outline thickness
    fillRect(px, surfW, surfH, ix, iy, iw, t, color);            // top
    fillRect(px, surfW, surfH, ix, iy + ih - t, iw, t, color);   // bottom
    fillRect(px, surfW, surfH, ix, iy, t, ih, color);            // left
    fillRect(px, surfW, surfH, ix + iw - t, iy, t, ih, color);   // right
}

void drawText(uint32_t *px, int surfW, int surfH, int x, int y, const char *text,
              uint32_t color) {
    for (const char *p = text; *p; ++p) {
        const Glyph *g = findGlyph(*p);
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if (g->col[col] & (1u << row)) {
                    fillRect(px, surfW, surfH, x + col * kTextScale, y + row * kTextScale,
                             kTextScale, kTextScale, color);
                }
            }
        }
        x += kTextAdvance;
    }
}

// Thick line via a square brush swept along the segment (software, no AA).
void drawLine(uint32_t *px, int surfW, int surfH, float x0, float y0, float x1,
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

} // namespace

void WaylandOverlay::paint(Buffer &buf) {
    // Stats text (top-left). fps <= 0 means "not provided".
    char stats[64];
    if (_fps > 0) {
        std::snprintf(stats, sizeof(stats), "det %.1f ms | %d fps", _detectMs, _fps);
    } else {
        std::snprintf(stats, sizeof(stats), "det %.1f ms", _detectMs);
    }
    const int statsLen = static_cast<int>(std::strlen(stats));
    IntRect statsRect{8, 8, 8 + statsLen * kTextAdvance + 2, 8 + kTextHeight + 2};

    // Pre-format each box's label so its text extent is part of the scene
    // bounds (the label can stick out far beyond the box outline).
    struct Item {
        const DetectionBox *box;
        uint32_t color;
        char label[96];
        int labelLen;
        int tx, ty; // label draw position
    };
    std::vector<Item> items;
    items.reserve(_boxes.size());

    // Boxes arrive in the captured output's pixel space; the layer surface can be
    // configured at a different size, so map them onto the surface once here.
    const int srcW = _sourceW.load(std::memory_order_relaxed) > 0 ? _sourceW.load(std::memory_order_relaxed) : _preferredW;
    const int srcH = _sourceH.load(std::memory_order_relaxed) > 0 ? _sourceH.load(std::memory_order_relaxed) : _preferredH;
    const float sx = (srcW > 0 && _width  > 0) ? static_cast<float>(_width)  / static_cast<float>(srcW) : 1.0f;
    const float sy = (srcH > 0 && _height > 0) ? static_cast<float>(_height) / static_cast<float>(srcH) : 1.0f;

    std::vector<DetectionBox> mapped;
    mapped.reserve(_boxes.size());
    for (const auto &b : _boxes) {
        DetectionBox m = b;
        m.x = b.x * sx;
        m.y = b.y * sy;
        m.w = b.w * sx;
        m.h = b.h * sy;
        mapped.push_back(m);
    }

    IntRect scene = statsRect;
    for (const auto &b : mapped) {
        if (!std::isfinite(b.x) || !std::isfinite(b.y) ||
            !std::isfinite(b.w) || !std::isfinite(b.h)) continue;

        Item it;
        it.box = &b;
        it.color = labelColor(b.label);
        const char *name = "?";
        if (b.label >= 0 && static_cast<size_t>(b.label) < _labels.size()) {
            name = _labels[static_cast<size_t>(b.label)].c_str();
        }
        std::snprintf(it.label, sizeof(it.label), "%s %.2f", name,
                      static_cast<double>(b.confidence));
        it.labelLen = static_cast<int>(std::strlen(it.label));
        it.tx = static_cast<int>(std::lround(b.x));
        it.ty = static_cast<int>(std::lround(b.y)) - kTextHeight - 2;
        if (it.ty < 0) it.ty = static_cast<int>(std::lround(b.y)) + 2; // inside the box
        items.push_back(it);

        IntRect r{static_cast<int>(std::floor(b.x)) - 3,
                  static_cast<int>(std::floor(b.y)) - 3,
                  static_cast<int>(std::ceil(b.x + b.w)) + 3,
                  static_cast<int>(std::ceil(b.y + b.h)) + 3};
        // +2: 1px text shadow + slack.
        r.add(IntRect{it.tx, it.ty, it.tx + it.labelLen * kTextAdvance + 2,
                      it.ty + kTextHeight + 2});
        scene.add(r);
    }
    // Extra primitives (HP/shield bars, skeleton bones) in the same source space.
    std::vector<Shape> shapes;
    shapes.reserve(_shapes.size());
    for (const auto &s : _shapes) {
        Shape m = s;
        m.x0 = s.x0 * sx;
        m.y0 = s.y0 * sy;
        m.x1 = s.x1 * sx;
        m.y1 = s.y1 * sy;
        m.thickness = s.thickness * ((sx + sy) * 0.5f);
        for (size_t i = 0; i + 1 < m.pts.size(); i += 2) {
            m.pts[i] *= sx;
            m.pts[i + 1] *= sy;
        }

        float minX = std::min(m.x0, m.x1), maxX = std::max(m.x0, m.x1);
        float minY = std::min(m.y0, m.y1), maxY = std::max(m.y0, m.y1);
        if (m.kind == Shape::Ellipse) {
            const float rx = std::fabs(m.x1), ry = std::fabs(m.y1);
            minX = m.x0 - rx; maxX = m.x0 + rx;
            minY = m.y0 - ry; maxY = m.y0 + ry;
        } else if (m.kind == Shape::Triangle || m.kind == Shape::Polygon) {
            if (!m.pts.empty()) {
                minX = maxX = m.pts[0];
                minY = maxY = m.pts[1];
                for (size_t i = 2; i + 1 < m.pts.size(); i += 2) {
                    minX = std::min(minX, m.pts[i]); maxX = std::max(maxX, m.pts[i]);
                    minY = std::min(minY, m.pts[i + 1]); maxY = std::max(maxY, m.pts[i + 1]);
                }
            }
        }

        shapes.push_back(m);
        IntRect r{static_cast<int>(std::floor(minX)) - 3,
                  static_cast<int>(std::floor(minY)) - 3,
                  static_cast<int>(std::ceil(maxX)) + 3,
                  static_cast<int>(std::ceil(maxY)) + 3};
        scene.add(r);
    }

    scene.clamp(_width, _height);

    // Clear: this buffer's stale pixels (from 2 frames ago) union the new scene.
    IntRect clearR;
    if (buf.hasPaint) clearR = IntRect{buf.x0, buf.y0, buf.x1, buf.y1};
    clearR.add(scene);
    clearR.clamp(_width, _height);
    clearRect(buf.data, _width, clearR);

    // Stats line (black shadow + white text).
    drawText(buf.data, _width, _height, 9, 9, stats, 0xFF000000);
    drawText(buf.data, _width, _height, 8, 8, stats, 0xFFFFFFFF);

    // Detection boxes + labels.
    for (const auto &it : items) {
        const DetectionBox &b = *it.box;
        drawBox(buf.data, _width, _height, b.x, b.y, b.w, b.h, it.color);
        drawText(buf.data, _width, _height, it.tx + 1, it.ty + 1, it.label, 0xFF000000);
        drawText(buf.data, _width, _height, it.tx, it.ty, it.label, it.color);
    }

    // Bars / bones on top of the boxes.
    for (const auto &s : shapes) {
        const int axis = (s.gradient == Shape::Vertical) ? 1 : 0;
        switch (s.kind) {
        case Shape::Line:
            drawLine(buf.data, _width, _height, s.x0, s.y0, s.x1, s.y1, s.thickness, s.color);
            break;
        case Shape::Ellipse:
            if (s.gradient != Shape::None) {
                fillEllipseGradient(buf.data, _width, _height, s.x0, s.y0, s.x1, s.y1,
                                    s.color, s.color2, axis);
            } else {
                fillEllipse(buf.data, _width, _height, s.x0, s.y0, s.x1, s.y1, s.color);
            }
            break;
        case Shape::Triangle:
        case Shape::Polygon:
            fillPolygon(buf.data, _width, _height, s.pts, s.color);
            break;
        case Shape::Filled:
        default:
            if (s.gradient != Shape::None) {
                fillRectGradient(buf.data, _width, _height,
                                 static_cast<int>(std::lround(s.x0)),
                                 static_cast<int>(std::lround(s.y0)),
                                 static_cast<int>(std::lround(s.x1 - s.x0)),
                                 static_cast<int>(std::lround(s.y1 - s.y0)),
                                 s.color, s.color2, axis);
            } else {
                fillRect(buf.data, _width, _height, static_cast<int>(std::lround(s.x0)),
                         static_cast<int>(std::lround(s.y0)),
                         static_cast<int>(std::lround(s.x1 - s.x0)),
                         static_cast<int>(std::lround(s.y1 - s.y0)), s.color);
            }
            break;
        }
    }

    buf.x0 = scene.x0;
    buf.y0 = scene.y0;
    buf.x1 = scene.x1;
    buf.y1 = scene.y1;
    buf.hasPaint = !scene.empty();

    // Damage: what changes on screen vs the previously COMMITTED frame — a
    // rect independent of which buffer carried it (the same buffer may be
    // re-committed when the compositor releases it immediately, in which
    // case the per-buffer rects alone would miss the previous scene).
    IntRect damage;
    if (_commitValid) damage = IntRect{_commitX0, _commitY0, _commitX1, _commitY1};
    damage.add(scene);
    damage.clamp(_width, _height);

    wl_surface_attach(_surface, buf.buffer, 0, 0);
    if (!damage.empty()) {
        wl_surface_damage_buffer(_surface, damage.x0, damage.y0,
                                 damage.x1 - damage.x0, damage.y1 - damage.y0);
    } else {
        // First attach (nothing on screen yet): damage a 1x1 so the commit
        // is still meaningful to the compositor.
        wl_surface_damage_buffer(_surface, 0, 0, 1, 1);
    }
    buf.busy = true;
    _lastAttached = &buf;
    wl_surface_commit(_surface);
    wl_display_flush(_display);

    _commitX0 = scene.x0;
    _commitY0 = scene.y0;
    _commitX1 = scene.x1;
    _commitY1 = scene.y1;
    _commitValid = !scene.empty();
}

void WaylandOverlay::maybePaint() {
    if (!_configured || _width <= 0 || _height <= 0) return;

    if (_sharedGen.load(std::memory_order_acquire) != _paintedGen) {
        {
            std::lock_guard<std::mutex> lk(_stateMutex);
            _boxes = _sharedBoxes;
            _shapes = _sharedShapes;
            _labels = _sharedLabels;
            _detectMs = _sharedDetectMs;
            _fps = _sharedFps;
            _paintedGen = _sharedGen.load(std::memory_order_relaxed);
        }
        _scenePending = true;
    }
    if (!_scenePending) return;

    // Prefer a free buffer that is not the one currently on screen (avoids
    // mutating pixels the compositor may still be sampling).
    Buffer *buf = nullptr;
    for (auto &b : _buffers) {
        if (!b.busy && b.data && &b != _lastAttached) {
            buf = &b;
            break;
        }
    }
    if (!buf) {
        for (auto &b : _buffers) {
            if (!b.busy && b.data) {
                buf = &b;
                break;
            }
        }
    }
    if (!buf) return; // both held by the compositor; a release event will wake us

    paint(*buf);
    _scenePending = false;
}

// ---------------------------------------------------------------------------
// Render thread + public API
// ---------------------------------------------------------------------------

void WaylandOverlay::threadMain() {
    const bool ok = initWayland();
    _initResult.set_value(ok);
    if (!ok) {
        teardown();
        return;
    }
    _running.store(true);
    std::cout << "[WaylandOverlay] Overlay running (" << _width << "x" << _height << ")"
              << std::endl;

    const int displayFd = wl_display_get_fd(_display);
    while (_threadRun.load(std::memory_order_relaxed) && _running.load()) {
        if (wl_display_dispatch_pending(_display) < 0) break;
        // prepare_read fails while events are still queued; dispatch and retry.
        if (wl_display_prepare_read(_display) != 0) continue;
        wl_display_flush(_display);

        struct pollfd pfds[2] = {
            {displayFd, POLLIN, 0},
            {_wakeFd, POLLIN, 0},
        };
        const int n = poll(pfds, 2, 100 /* ms */);
        if (n > 0 && (pfds[0].revents & POLLIN)) {
            wl_display_read_events(_display);
        } else {
            wl_display_cancel_read(_display);
        }
        if (wl_display_dispatch_pending(_display) < 0) break;
        if (n > 0 && (pfds[1].revents & POLLIN)) {
            uint64_t v;
            (void)!read(_wakeFd, &v, sizeof(v)); // drain
        }
        maybePaint();
    }

    teardown();
    _running.store(false);
}

WaylandOverlay::~WaylandOverlay() { stop(); }

bool WaylandOverlay::start() {
    if (_running.load()) return true;
    _wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (_wakeFd < 0) {
        std::cerr << "[WaylandOverlay] eventfd failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    _threadRun.store(true);
    _thread = std::thread(&WaylandOverlay::threadMain, this);
    const bool ok = _initResult.get_future().get();
    if (!ok) stop();
    return ok;
}

void WaylandOverlay::stop() {
    _threadRun.store(false);
    if (_thread.joinable()) {
        if (_wakeFd >= 0) {
            const uint64_t one = 1;
            (void)!write(_wakeFd, &one, sizeof(one));
        }
        _thread.join();
    }
    if (_wakeFd >= 0) {
        close(_wakeFd);
        _wakeFd = -1;
    }
    _running.store(false);
}

void WaylandOverlay::setLabelNames(const std::vector<std::string> &names) {
    std::lock_guard<std::mutex> lk(_stateMutex);
    _sharedLabels = names;
}

void WaylandOverlay::setSourceSize(int w, int h) {
    _sourceW.store(w > 0 ? w : 0, std::memory_order_relaxed);
    _sourceH.store(h > 0 ? h : 0, std::memory_order_relaxed);
}

void WaylandOverlay::setDetections(std::vector<DetectionBox> boxes) {
    if (!_running.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lk(_stateMutex);
        _sharedBoxes = std::move(boxes);
        _sharedGen.fetch_add(1, std::memory_order_release);
    }
    const uint64_t one = 1;
    (void)!write(_wakeFd, &one, sizeof(one));
}

void WaylandOverlay::setShapes(std::vector<Shape> shapes) {
    if (!_running.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lk(_stateMutex);
        _sharedShapes = std::move(shapes);
        _sharedGen.fetch_add(1, std::memory_order_release);
    }
    const uint64_t one = 1;
    (void)!write(_wakeFd, &one, sizeof(one));
}

void WaylandOverlay::setStats(double detectMs, int fps) {
    if (!_running.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lk(_stateMutex);
        _sharedDetectMs = detectMs;
        _sharedFps = fps;
        _sharedGen.fetch_add(1, std::memory_order_release);
    }
    const uint64_t one = 1;
    (void)!write(_wakeFd, &one, sizeof(one));
}

// ---------------------------------------------------------------------------
// Interactive menu — app-facing API
// ---------------------------------------------------------------------------

bool WaylandOverlay::setMenuVisible(bool visible) {
    if (!_running.load(std::memory_order_relaxed)) return false;
    {
        std::lock_guard<std::mutex> lk(_stateMutex);
        _sharedMenuVisible = visible;
        _sharedGen.fetch_add(1, std::memory_order_release);
    }
    const uint64_t one = 1;
    (void)!write(_wakeFd, &one, sizeof(one));
    return visible;
}

void WaylandOverlay::registerSetting(int id, const std::string &name, bool initial) {
    {
        std::lock_guard<std::mutex> lk(_stateMutex);
        MenuItem item;
        item.type = MenuItem::Toggle;
        item.id = id;
        item.name = name;
        item.min = 0.0f;
        item.max = 1.0f;
        item.step = 1.0f;
        item.value = initial ? 1.0f : 0.0f;
        bool found = false;
        for (auto &s : _sharedSettings) {
            if (s.id == id) { s = item; found = true; break; }
        }
        if (!found) _sharedSettings.push_back(std::move(item));
        _sharedGen.fetch_add(1, std::memory_order_release);
    }
    if (_running.load(std::memory_order_relaxed)) {
        const uint64_t one = 1;
        (void)!write(_wakeFd, &one, sizeof(one));
    }
}

void WaylandOverlay::registerSetting(int id, const std::string &name, float min,
                                     float max, float step, float initial) {
    {
        std::lock_guard<std::mutex> lk(_stateMutex);
        MenuItem item;
        item.type = MenuItem::Slider;
        item.id = id;
        item.name = name;
        item.min = min;
        item.max = max;
        item.step = step;
        item.value = initial;
        bool found = false;
        for (auto &s : _sharedSettings) {
            if (s.id == id) { s = item; found = true; break; }
        }
        if (!found) _sharedSettings.push_back(std::move(item));
        _sharedGen.fetch_add(1, std::memory_order_release);
    }
    if (_running.load(std::memory_order_relaxed)) {
        const uint64_t one = 1;
        (void)!write(_wakeFd, &one, sizeof(one));
    }
}

void WaylandOverlay::setSetting(int id, float value) {
    {
        std::lock_guard<std::mutex> lk(_stateMutex);
        for (auto &s : _sharedSettings) {
            if (s.id == id) { s.value = value; break; }
        }
        _sharedGen.fetch_add(1, std::memory_order_release);
    }
    if (_running.load(std::memory_order_relaxed)) {
        const uint64_t one = 1;
        (void)!write(_wakeFd, &one, sizeof(one));
    }
}

bool WaylandOverlay::pollMenuEvent(MenuEvent &out) {
    std::lock_guard<std::mutex> lk(_eventMutex);
    if (_events.empty()) return false;
    out = _events.front();
    _events.pop_front();
    return true;
}
