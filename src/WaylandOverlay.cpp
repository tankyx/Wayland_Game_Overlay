#ifndef _GNU_SOURCE
#define _GNU_SOURCE // memfd_create, eventfd
#endif

#include "WaylandOverlay.h"

#include <wayland-client.h>
#include "wayland/xdg-shell-client-protocol.h"
#include "wayland/wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wayland/fractional-scale-v1-client-protocol.h"
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
    } else if (std::strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        // Optional: present on compositors with fractional-scale support.
        // Absent => _logicalScale falls back to the integer wl_output.scale.
        self->_fractionalScaleManager = static_cast<wp_fractional_scale_manager_v1 *>(
            wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1));
    }
}

void WaylandOverlay::fractionalPreferredScale(void *data, wp_fractional_scale_v1 *,
                                              uint32_t scale) {
    auto *self = static_cast<WaylandOverlay *>(data);
    if (scale <= 0) return;

    self->_logicalScale = static_cast<float>(scale) / 120.0f;
    const int bufferScale = static_cast<int>(std::ceil(self->_logicalScale));
    if (bufferScale != self->_bufferScale) {
        self->_bufferScale = bufferScale;
        if (self->_surface) wl_surface_set_buffer_scale(self->_surface, bufferScale);
    }

    // The first preferred_scale can arrive only once the surface is mapped
    // (after the initial pin), and the scale can change at runtime, so re-apply
    // the pin: margins/size are logical and depend on _logicalScale. No-op while
    // _layerSurface is still null during init — initWayland applies it after.
    if (self->_layerSurface) {
        self->applyWindowRect();
        wl_surface_commit(self->_surface);
        self->_scenePending = true;
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
    self->_ptrX = wl_fixed_to_int(x);
    self->_ptrY = wl_fixed_to_int(y);
    self->_menuHover = self->rowAt(self->_ptrX, self->_ptrY);
    self->_scenePending = true;
}

void WaylandOverlay::pointerLeave(void *data, wl_pointer *, uint32_t, wl_surface *) {
    auto *self = static_cast<WaylandOverlay *>(data);
    self->_menuHover = -1;
    self->_pointerDrag = false;
    self->_scenePending = true;
}

void WaylandOverlay::pointerMotion(void *data, wl_pointer *, uint32_t, int32_t x, int32_t y) {
    auto *self = static_cast<WaylandOverlay *>(data);
    self->_ptrX = wl_fixed_to_int(x);
    self->_ptrY = wl_fixed_to_int(y);
    if (self->_pointerDrag) {
        self->setSliderFromX(self->_menuFocus, self->_ptrX);
    } else {
        self->_menuHover = self->rowAt(self->_ptrX, self->_ptrY);
    }
    self->_scenePending = true;
}

void WaylandOverlay::pointerButton(void *data, wl_pointer *, uint32_t, uint32_t,
                                   uint32_t button, uint32_t state) {
    auto *self = static_cast<WaylandOverlay *>(data);
    if (button != BTN_LEFT) return;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        const int idx = self->rowAt(self->_ptrX, self->_ptrY);
        if (idx < 0) return; // click outside the menu -> falls through to the game
        self->_menuFocus = idx;
        const MenuItem &row = self->_menuRows[idx];
        if (row.type == MenuItem::Slider) {
            self->_pointerDrag = true;
            self->setSliderFromX(idx, self->_ptrX);
        } else {
            self->activateRow(idx);
        }
    } else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        self->_pointerDrag = false;
    }
    self->_scenePending = true;
}

void WaylandOverlay::keyboardKey(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key,
                                 uint32_t state) {
    auto *self = static_cast<WaylandOverlay *>(data);
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !self->_menuVisible) return;
    const MenuKey mk = menuKeyFor(key - 8);
    switch (mk) {
    case MenuKey::Up:
        if (self->_menuFocus > 0) self->_menuFocus--;
        break;
    case MenuKey::Down:
        if (self->_menuFocus < static_cast<int>(self->_menuRows.size()) - 1) self->_menuFocus++;
        break;
    case MenuKey::Left:
    case MenuKey::Right: {
        const int i = self->_menuFocus;
        if (i >= 0 && i < static_cast<int>(self->_menuRows.size()) &&
            self->_menuRows[i].type == MenuItem::Slider) {
            MenuItem &row = self->_menuRows[i];
            const float d = (mk == MenuKey::Left) ? -row.step : row.step;
            row.value = clampStep(row.value + d, row.min, row.max, row.step);
            self->commitRowValue(i);
        }
        break;
    }
    case MenuKey::Activate:
        self->activateRow(self->_menuFocus);
        break;
    case MenuKey::Close:
        self->_menuVisible = false;
        self->applyInputRegion();
        break;
    case MenuKey::Unknown:
    default:
        break;
    }
    self->_scenePending = true;
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

void WaylandOverlay::outputScale(void *data, wl_output *, int32_t scale) {
    static_cast<OutputInfo *>(data)->scale = scale;
}

void WaylandOverlay::layerConfigure(void *data, zwlr_layer_surface_v1 *ls,
                                  uint32_t serial, uint32_t w, uint32_t h) {
    auto *self = static_cast<WaylandOverlay *>(data);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (w > 0 && h > 0 &&
        (static_cast<int>(w) * self->_bufferScale != self->_width ||
         static_cast<int>(h) * self->_bufferScale != self->_height)) {
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
    const int pw = w * _bufferScale; // physical buffer size
    const int ph = h * _bufferScale;
    std::cout << "[WaylandOverlay] Layer surface configured: " << w << "x" << h
              << " (buffer scale " << _bufferScale << " -> " << pw << "x" << ph << " px)"
              << std::endl;
    for (auto &b : _buffers) destroyBuffer(b);
    _lastAttached = nullptr;
    _width = pw;
    _height = ph;
    for (auto &b : _buffers) initBuffer(b, pw, ph);
}

// ---------------------------------------------------------------------------
// Setup / teardown (render thread)
// ---------------------------------------------------------------------------

WaylandOverlay::OutputInfo *WaylandOverlay::pickOutput() const {
    const char *envName = std::getenv("WAYLAND_OVERLAY_OUTPUT");
    if (envName && *envName) {
        for (auto *o : _outputs) {
            if (o->name == envName) return o;
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
                return o;
            }
        }
    }
    // Otherwise fall back to the largest output rather than letting the
    // compositor pick the (possibly wrong) focused one.
    OutputInfo *best = nullptr;
    long bestArea = -1;
    for (auto *o : _outputs) {
        const long area = static_cast<long>(o->width) * o->height;
        if (area > bestArea) {
            bestArea = area;
            best = o;
        }
    }
    if (best) {
        std::cout << "[WaylandOverlay] Using largest output (no exact capture-size match)" << std::endl;
    }
    return best; // nullptr lets the compositor decide (typically the focused output).
}

void WaylandOverlay::applyWindowRect() {
    if (!_layerSurface) return;
    if (_winRect && !coversWholeOutput()) {
        zwlr_layer_surface_v1_set_anchor(_layerSurface,
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
        zwlr_layer_surface_v1_set_margin(
            _layerSurface, static_cast<int>(std::lroundf(_winY / _logicalScale)), 0, 0,
            static_cast<int>(std::lroundf(_winX / _logicalScale)));
        zwlr_layer_surface_v1_set_size(
            _layerSurface, static_cast<int>(std::lroundf(_winW / _logicalScale)),
            static_cast<int>(std::lroundf(_winH / _logicalScale)));
    } else {
        zwlr_layer_surface_v1_set_anchor(_layerSurface,
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(_layerSurface, 0, 0);
    }
}

bool WaylandOverlay::coversWholeOutput() const {
    return _chosenW > 0 && _chosenH > 0 && _winX == 0 && _winY == 0 &&
           _winW == _chosenW && _winH == _chosenH;
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

    // Pick the output; record its connector + mode and seed the logical scale
    // from the integer wl_output.scale (preferred_scale may override below).
    OutputInfo *out = pickOutput();
    _chosenName = out ? out->name : "";
    _chosenW = out ? out->width : 0;
    _chosenH = out ? out->height : 0;
    _logicalScale = out ? static_cast<float>(out->scale) : 1.0f;
    if (_logicalScale < 1.0f) _logicalScale = 1.0f;

    _surface = wl_compositor_create_surface(_compositor);

    // Empty input region => fully click-through.
    wl_region *region = wl_compositor_create_region(_compositor);
    wl_surface_set_input_region(_surface, region);
    wl_region_destroy(region);

    // Bind fractional-scale before the first commit so preferred_scale arrives
    // before we size the surface (it drives every physical->logical conversion).
    if (_fractionalScaleManager) {
        _fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(
            _fractionalScaleManager, _surface);
        static const wp_fractional_scale_v1_listener fsListener = {
            &WaylandOverlay::fractionalPreferredScale,
        };
        wp_fractional_scale_v1_add_listener(_fractionalScale, &fsListener, this);
    }
    wl_display_roundtrip(_display); // receive preferred_scale (if any)

    _bufferScale = static_cast<int>(std::ceil(_logicalScale));
    if (_bufferScale < 1) _bufferScale = 1;
    wl_surface_set_buffer_scale(_surface, _bufferScale);

    _layerSurface = zwlr_layer_shell_v1_get_layer_surface(
        _layerShell, _surface, out ? out->output : nullptr,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wayland-overlay");
    if (!_layerSurface) {
        std::cerr << "[WaylandOverlay] Failed to create layer surface" << std::endl;
        return false;
    }

    // Sync any pre-start window rect, then position the surface (pin vs fill).
    {
        std::lock_guard<std::mutex> lk(_stateMutex);
        _winX = _sharedWinX;
        _winY = _sharedWinY;
        _winW = _sharedWinW;
        _winH = _sharedWinH;
        _winRect = _sharedWinRect;
        _winGen = _sharedWinGen.load(std::memory_order_relaxed);
    }
    applyWindowRect();
    // Negative exclusive zone lets us render over panels and fullscreen windows.
    zwlr_layer_surface_v1_set_exclusive_zone(_layerSurface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(_layerSurface, 0);

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
    if (_fractionalScale) {
        wp_fractional_scale_v1_destroy(_fractionalScale);
        _fractionalScale = nullptr;
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
    if (_fractionalScaleManager) {
        wp_fractional_scale_manager_v1_destroy(_fractionalScaleManager);
        _fractionalScaleManager = nullptr;
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

// Menu geometry (surface pixels).
constexpr int kMenuWidth = 320;
constexpr int kMenuMargin = 16;
constexpr int kMenuRowH = 28;
constexpr int kMenuTitleH = 22;
constexpr int kMenuPad = 8;

uint32_t dim(uint32_t color, float alpha) {
    if (alpha >= 1.0f) return color;
    const uint32_t a = static_cast<uint32_t>(((color >> 24) & 0xFF) * alpha);
    return (color & 0x00FFFFFF) | (a << 24);
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

    IntRect scene;
    if (_showStats) scene.add(statsRect);
    if (_showBoxes) {
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
    }
    // Extra primitives in the same source space.
    std::vector<Shape> shapes;
    shapes.reserve(_shapes.size());
    if (_showShapes) {
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
    }

    if (_menuVisible && _menuW > 0) {
        scene.add(IntRect{_menuX, _menuY, _menuX + _menuW, _menuY + _menuH});
    }
    scene.clamp(_width, _height);

    // Clear: this buffer's stale pixels (from 2 frames ago) union the new scene.
    IntRect clearR;
    if (buf.hasPaint) clearR = IntRect{buf.x0, buf.y0, buf.x1, buf.y1};
    clearR.add(scene);
    clearR.clamp(_width, _height);
    clearRect(buf.data, _width, clearR);

    // Stats line (black shadow + white text).
    if (_showStats) {
        drawText(buf.data, _width, _height, 9, 9, stats, dim(0xFF000000, _opacity));
        drawText(buf.data, _width, _height, 8, 8, stats, dim(0xFFFFFFFF, _opacity));
    }

    // Detection boxes + labels.
    if (_showBoxes) {
        for (const auto &it : items) {
            const DetectionBox &b = *it.box;
            const uint32_t c = dim(it.color, _opacity);
            drawBox(buf.data, _width, _height, b.x, b.y, b.w, b.h, c);
            drawText(buf.data, _width, _height, it.tx + 1, it.ty + 1, it.label, dim(0xFF000000, _opacity));
            drawText(buf.data, _width, _height, it.tx, it.ty, it.label, c);
        }
    }

    // Extra primitives on top of the boxes.
    if (_showShapes) {
        for (const auto &s : shapes) {
            const int axis = (s.gradient == Shape::Vertical) ? 1 : 0;
            const uint32_t c = dim(s.color, _opacity);
            const uint32_t c2 = dim(s.color2, _opacity);
            switch (s.kind) {
            case Shape::Line:
                drawLine(buf.data, _width, _height, s.x0, s.y0, s.x1, s.y1, s.thickness, c);
                break;
            case Shape::Ellipse:
                if (s.outline) {
                    fillEllipseOutline(buf.data, _width, _height, s.x0, s.y0, s.x1, s.y1,
                                       s.thickness, c);
                } else if (s.gradient != Shape::NoGradient) {
                    fillEllipseGradient(buf.data, _width, _height, s.x0, s.y0, s.x1, s.y1, c, c2, axis);
                } else {
                    fillEllipse(buf.data, _width, _height, s.x0, s.y0, s.x1, s.y1, c);
                }
                break;
            case Shape::Triangle:
            case Shape::Polygon:
                if (s.outline) {
                    strokePolygon(buf.data, _width, _height, s.pts, s.thickness, c);
                } else {
                    fillPolygon(buf.data, _width, _height, s.pts, c);
                }
                break;
            case Shape::Filled:
            default:
                if (s.outline) {
                    fillRectOutline(buf.data, _width, _height,
                                    static_cast<int>(std::lround(s.x0)),
                                    static_cast<int>(std::lround(s.y0)),
                                    static_cast<int>(std::lround(s.x1 - s.x0)),
                                    static_cast<int>(std::lround(s.y1 - s.y0)),
                                    static_cast<int>(std::lround(s.thickness)), c);
                } else if (s.gradient != Shape::NoGradient) {
                    fillRectGradient(buf.data, _width, _height,
                                     static_cast<int>(std::lround(s.x0)),
                                     static_cast<int>(std::lround(s.y0)),
                                     static_cast<int>(std::lround(s.x1 - s.x0)),
                                     static_cast<int>(std::lround(s.y1 - s.y0)),
                                     c, c2, axis);
                } else {
                    fillRect(buf.data, _width, _height, static_cast<int>(std::lround(s.x0)),
                             static_cast<int>(std::lround(s.y0)),
                             static_cast<int>(std::lround(s.x1 - s.x0)),
                             static_cast<int>(std::lround(s.y1 - s.y0)), c);
                }
                break;
            }
        }
    }

    // Interactive menu (drawn last, on top; full opacity).
    if (_menuVisible && _menuW > 0) {
        fillRect(buf.data, _width, _height, _menuX, _menuY, _menuW, _menuH, 0xE0000000);
        drawBox(buf.data, _width, _height, _menuX, _menuY, _menuW, _menuH, 0xFFAAAAAA);
        drawText(buf.data, _width, _height, _menuX + kMenuPad, _menuY + kMenuPad, "Overlay", 0xFFFFFFFF);

        const int rx = _menuX + kMenuPad;
        int ty = _menuY + kMenuPad + kMenuTitleH;
        for (size_t i = 0; i < _menuRows.size(); ++i) {
            const MenuItem &row = _menuRows[i];
            const bool focused = (static_cast<int>(i) == _menuFocus);
            const bool hovered = (static_cast<int>(i) == _menuHover);
            if (focused) {
                fillRect(buf.data, _width, _height, rx - 2, ty - 2, _menuW - 2 * kMenuPad + 4, kMenuRowH - 2, 0x30FFFFFF);
            } else if (hovered) {
                fillRect(buf.data, _width, _height, rx - 2, ty - 2, _menuW - 2 * kMenuPad + 4, kMenuRowH - 2, 0x18FFFFFF);
            }

            char label[128];
            if (row.type == MenuItem::Toggle) {
                std::snprintf(label, sizeof(label), "%s: %s", row.name.c_str(),
                              row.value > 0.5f ? "ON" : "OFF");
            } else if (row.type == MenuItem::Slider) {
                std::snprintf(label, sizeof(label), "%s: %.0f", row.name.c_str(), row.value);
            } else {
                std::snprintf(label, sizeof(label), "%s", row.name.c_str());
            }
            drawText(buf.data, _width, _height, rx, ty + 6, label, 0xFFFFFFFF);

            if (row.type == MenuItem::Slider) {
                const int trackMid = ty + 6 + kTextHeight / 2;
                const int trackX = rx + 9 * kTextAdvance;
                const int trackEnd = _menuX + _menuW - kMenuPad;
                fillRect(buf.data, _width, _height, trackX, trackMid - 1, trackEnd - trackX, 2, 0xFF888888);
                float frac = (row.max > row.min) ? (row.value - row.min) / (row.max - row.min) : 0.0f;
                frac = std::min(1.0f, std::max(0.0f, frac));
                const int thumbX = trackX + static_cast<int>(frac * (trackEnd - trackX - 8));
                fillRect(buf.data, _width, _height, thumbX, trackMid - 6, 8, 12, 0xFFFFFFFF);
            }
            ty += kMenuRowH;
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
    if (_sharedMenuGen.load(std::memory_order_acquire) != _menuGen) {
        bool visChanged = false;
        {
            std::lock_guard<std::mutex> lk(_stateMutex);
            visChanged = (_sharedMenuVisible != _menuVisible);
            _menuVisible = _sharedMenuVisible;
            _settings = _sharedSettings;
            _menuGen = _sharedMenuGen.load(std::memory_order_relaxed);
        }
        rebuildRows();
        if (visChanged) {
            layoutMenu();
            applyInputRegion();
        }
        _scenePending = true;
    }
    if (_sharedWinGen.load(std::memory_order_acquire) != _winGen) {
        {
            std::lock_guard<std::mutex> lk(_stateMutex);
            _winX = _sharedWinX;
            _winY = _sharedWinY;
            _winW = _sharedWinW;
            _winH = _sharedWinH;
            _winRect = _sharedWinRect;
            _winGen = _sharedWinGen.load(std::memory_order_relaxed);
        }
        applyWindowRect();
        wl_surface_commit(_surface);
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
// Menu model + input region (render thread)
// ---------------------------------------------------------------------------

void WaylandOverlay::rebuildRows() {
    _menuRows.clear();
    _menuRows.reserve(_settings.size() + 6);
    auto push = [&](MenuItem::Type type, int id, const char *name, float min, float max,
                    float step, float value) {
        MenuItem m;
        m.type = type;
        m.id = id;
        m.name = name;
        m.min = min;
        m.max = max;
        m.step = step;
        m.value = value;
        _menuRows.push_back(std::move(m));
    };
    push(MenuItem::Toggle, -1, "Boxes", 0, 1, 1, _showBoxes ? 1.0f : 0.0f);
    push(MenuItem::Toggle, -2, "Shapes", 0, 1, 1, _showShapes ? 1.0f : 0.0f);
    push(MenuItem::Toggle, -3, "Stats", 0, 1, 1, _showStats ? 1.0f : 0.0f);
    push(MenuItem::Slider, -4, "Opacity", 0, 100, 1, _opacity * 100.0f);
    for (const auto &s : _settings) _menuRows.push_back(s);
    push(MenuItem::Button, -5, "Quit", 0, 0, 0, 0);
    push(MenuItem::Button, -6, "Close", 0, 0, 0, 0);
    if (_menuFocus >= static_cast<int>(_menuRows.size()))
        _menuFocus = static_cast<int>(_menuRows.size()) - 1;
    if (_menuFocus < 0) _menuFocus = 0;
}

void WaylandOverlay::layoutMenu() {
    _menuW = kMenuWidth;
    _menuH = kMenuPad * 2 + kMenuTitleH + static_cast<int>(_menuRows.size()) * kMenuRowH;
    _menuX = _width - _menuW - kMenuMargin;
    _menuY = kMenuMargin;
}

void WaylandOverlay::applyInputRegion() {
    if (!_surface || !_compositor) return;
    wl_region *region = wl_compositor_create_region(_compositor);
    if (_menuVisible) {
        wl_region_add(region, _menuX, _menuY, _menuW, _menuH);
    }
    wl_surface_set_input_region(_surface, region);
    wl_region_destroy(region);
    if (_layerSurface) {
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            _layerSurface,
            _menuVisible ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
                         : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    }
}

int WaylandOverlay::rowAt(int x, int y) const {
    if (!_menuVisible) return -1;
    if (x < _menuX + kMenuPad || x >= _menuX + _menuW - kMenuPad) return -1;
    int top = _menuY + kMenuPad + kMenuTitleH;
    for (size_t i = 0; i < _menuRows.size(); ++i) {
        if (y >= top && y < top + kMenuRowH) return static_cast<int>(i);
        top += kMenuRowH;
    }
    return -1;
}

void WaylandOverlay::enqueueEvent(MenuEvent e) {
    std::lock_guard<std::mutex> lk(_eventMutex);
    _events.push_back(e);
}

void WaylandOverlay::activateRow(int index) {
    if (index < 0 || index >= static_cast<int>(_menuRows.size())) return;
    MenuItem &row = _menuRows[index];
    if (row.type == MenuItem::Toggle) {
        row.value = row.value > 0.5f ? 0.0f : 1.0f;
        commitRowValue(index);
    } else if (row.type == MenuItem::Button) {
        if (row.id == -5) {
            enqueueEvent(MenuEvent{MenuEvent::Quit, -1, 0.0f});
        } else if (row.id == -6) {
            _menuVisible = false;
            applyInputRegion();
        }
    }
}

void WaylandOverlay::commitRowValue(int index) {
    MenuItem &row = _menuRows[index];
    switch (row.id) {
    case -1: _showBoxes = row.value > 0.5f; break;
    case -2: _showShapes = row.value > 0.5f; break;
    case -3: _showStats = row.value > 0.5f; break;
    case -4: _opacity = row.value / 100.0f; break;
    default:
        if (row.id >= 0) {
            for (auto &s : _settings) {
                if (s.id == row.id) s.value = row.value;
            }
            {
                std::lock_guard<std::mutex> lk(_stateMutex);
                for (auto &s : _sharedSettings) {
                    if (s.id == row.id) s.value = row.value;
                }
            }
            enqueueEvent(MenuEvent{MenuEvent::SettingChanged, row.id, row.value});
        }
        break;
    }
}

void WaylandOverlay::setSliderFromX(int index, int x) {
    if (index < 0 || index >= static_cast<int>(_menuRows.size())) return;
    MenuItem &row = _menuRows[index];
    const int trackX = _menuX + kMenuPad + 9 * kTextAdvance;
    const int trackEnd = _menuX + _menuW - kMenuPad;
    float frac = (trackEnd > trackX)
                     ? static_cast<float>(x - trackX) / static_cast<float>(trackEnd - trackX)
                     : 0.0f;
    frac = std::min(1.0f, std::max(0.0f, frac));
    row.value = clampStep(row.min + frac * (row.max - row.min), row.min, row.max, row.step);
    commitRowValue(index);
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

void WaylandOverlay::setWindowRect(int x, int y, int w, int h) {
    {
        std::lock_guard<std::mutex> lk(_stateMutex);
        _sharedWinX = x;
        _sharedWinY = y;
        _sharedWinW = w;
        _sharedWinH = h;
        _sharedWinRect = (w > 0 && h > 0);
    }
    _sharedWinGen.fetch_add(1, std::memory_order_release);
    if (_running.load(std::memory_order_relaxed)) {
        const uint64_t one = 1;
        (void)!write(_wakeFd, &one, sizeof(one));
    }
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
        _sharedMenuGen.fetch_add(1, std::memory_order_release);
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
        _sharedMenuGen.fetch_add(1, std::memory_order_release);
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
        _sharedMenuGen.fetch_add(1, std::memory_order_release);
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
        _sharedMenuGen.fetch_add(1, std::memory_order_release);
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
