# Wayland Game Overlay Library — Design Spec

Date: 2026-10-13
Status: Approved (design), pending spec review

## Goal

Extract the click-through Wayland overlay, originally duplicated as two private
copies, into a single reusable C++ class that any developer can consume in their
own game-overlay project.

The two private copies were:

- a base version (boxes, labels, stats)
- a strict superset (adds `Shape` primitives: filled rect + line)

The superset copy is the canonical implementation; the base was copied verbatim
and then extended.

## Non-goals

- No Windows support.
- No screen capture. The library renders only; capture stays in the apps.
- No X11 game-window detection; that stays in the consuming app.
- No install rules / packaging for distros. Consumption is via
  `add_subdirectory` or `FetchContent`.

## Deliverables

### 1. New library repo `Wayland_Game_Overlay`

```
Wayland_Game_Overlay/
├── CMakeLists.txt          # wayland_overlay static lib + example target
├── README.md               # consumption docs (add_subdirectory / FetchContent)
├── include/
│   └── WaylandOverlay.h    # public API
├── src/
│   ├── WaylandOverlay.cpp  # the overlay implementation
│   └── wayland/            # vendored generated bindings
│       ├── xdg-shell-protocol.c, xdg-shell-client-protocol.h
│       └── wlr-layer-shell-unstable-v1-protocol.c, -client-protocol.h
└── example/
    └── main.cpp            # standalone demo
```

CMake target `wayland_overlay` (STATIC):

- Sources: `WaylandOverlay.cpp` + the two vendored `.c` files.
- `target_include_directories(wayland_overlay PUBLIC include)`
- `pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)`
- `target_link_libraries(wayland_overlay PUBLIC PkgConfig::WAYLAND pthread)`
- No X11 dependency. No install rules.
- C++17, matching both consumers.

### 2. Public API — `WaylandOverlay`

The public class:

```cpp
class WaylandOverlay {
public:
    struct DetectionBox { float x, y, w, h; int label; float confidence; };
    struct Shape {
        enum Kind { Filled, Line };
        Kind kind = Filled;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Filled: rect corners; Line: endpoints
        float thickness = 1.0f;               // Line only
        uint32_t color = 0xFFFFFFFF;          // 0xAARRGGBB
    };

    WaylandOverlay() = default;
    explicit WaylandOverlay(int screenW, int screenH);
    ~WaylandOverlay();
    WaylandOverlay(const WaylandOverlay&) = delete;
    WaylandOverlay& operator=(const WaylandOverlay&) = delete;

    bool start();
    void stop();
    bool isRunning() const;

    void setLabelNames(const std::vector<std::string>& names);
    void setDetections(std::vector<DetectionBox> boxes);
    void setShapes(std::vector<Shape> shapes);
    void setStats(double detectMs, int fps);
    void setSourceSize(int w, int h);
};
```

Semantics unchanged from the current implementation:

- Fullscreen, click-through wlr-layer-shell surface on the overlay layer
  (KWin + wlroots), wl_shm ARGB8888 buffers painted in software (no cairo/EGL).
- Dedicated render thread owns the Wayland connection and event dispatch.
- Setters are thread-safe: swap state under a short mutex, bump a generation
  counter, poke an eventfd that wakes the render thread.
- Double-buffered (two wl_buffers), damage-tracked paint.
- Output chosen by matching wl_output name/size against the preferred
  screenW/screenH passed to the constructor (falls back to largest output).
- Boxes are 2px opaque outlines with a per-label color from an 8-entry palette,
  a "name 0.87" label in a built-in 5x7 bitmap font, plus a stats line (top-left).
- `setSourceSize` maps incoming box coordinates onto the layer surface when the
  surface is configured at a different size than the captured output.

### 3. Migration of the two consumers

**The base consumer**

- Delete its private overlay copy (`<old overlay header/cpp>`, `wayland/`).
- CMake Linux branch: remove the old overlay sources from its executable; add
  `add_subdirectory(../Wayland_Game_Overlay wayland_overlay_build)` and link
  `wayland_overlay`.
- Its main source: `#include <WaylandOverlay.h>` and alias `WaylandOverlay`
  (any Windows overlay branch untouched).

**The superset consumer**

- Delete its private overlay copy (`<old overlay header/cpp>`, `wayland/`).
- CMake: remove those sources; add the same `add_subdirectory` + link; keep its
  app-specific window-detection header and `HAVE_X11` block.
- Its main source: `#include <WaylandOverlay.h>` and use the `WaylandOverlay`
  class (including `Shape` references).

### 4. Example and verification

- `example/main.cpp`: open the overlay on the largest output, paint a few boxes,
  one line, one filled rect, and a stats line; run a few seconds, then `stop()`.
- Verification gate: build the example standalone; then build both consumers and
  confirm they compile and link against `wayland_overlay`. Runtime testing of the
  overlay requires a live Wayland session and is out of scope for CI-style
  verification.

## Error handling and threading

No changes. The implementation is the already-shipped, working code from two
projects. This task is a pure extraction + rename + relink.
