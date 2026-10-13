# Wayland Game Overlay

A reusable, dependency-light C++ class for drawing a click-through debug
overlay on Wayland — the kind you put over a game to draw detection boxes,
HP bars, skeletons, or stats.

- Fullscreen wlr-layer-shell surface on the **overlay** layer (KWin + wlroots),
  so it floats above the game and never steals input.
- Software-painted `wl_shm` ARGB8888 buffers — no cairo, no EGL, no GPU.
- Thread-safe: a dedicated render thread owns the Wayland connection; your
  detection thread just calls setters.
- Only runtime dependency is `wayland-client` (+ `pthread`). The wlr-layer-shell
  and xdg-shell protocol bindings are vendored and compiled in.

---

## Quick start

### Prerequisites

- A Wayland session (KWin or wlroots). `$WAYLAND_DISPLAY` must be set when the
  app runs.
- Build deps: CMake ≥ 3.16, a C++17 compiler, `pkg-config`, and the
  `wayland-client` dev package (`libwayland-dev` on Debian/Ubuntu).

### 1. Add it to your CMake project

```cmake
add_subdirectory(path/to/Wayland_Game_Overlay wayland_overlay_build)
target_link_libraries(your_app PRIVATE wayland_overlay)
```

`wayland-client` and `pthread` are linked transitively — you don't need to find
them yourself.

<details>
<summary>FetchContent alternative</summary>

```cmake
include(FetchContent)
FetchContent_Declare(wayland_game_overlay
    GIT_REPOSITORY https://github.com/<you>/Wayland_Game_Overlay
    GIT_TAG main)
FetchContent_MakeAvailable(wayland_game_overlay)
target_link_libraries(your_app PRIVATE wayland_overlay)
```
</details>

### 2. Drive it from your code

```cpp
#include <WaylandOverlay.h>

WaylandOverlay overlay(1920, 1080); // preferred output size → picks the monitor
if (!overlay.start()) {
    // no Wayland session — fail soft, keep your app running
}

overlay.setLabelNames({"enemy", "friend"});
overlay.setDetections({{100, 200, 50, 100, 0, 0.92f}});
overlay.setShapes({{WaylandOverlay::Shape::Filled, 10, 10, 200, 30, 0, 0xFF303030}});
overlay.setStats(3.2, 60);          // detect ms, fps → top-left stats line
overlay.setSourceSize(1920, 1080);  // the space your coordinates are expressed in

// ... your loop (capture, detect, publish) ...
overlay.stop();
```

That's the whole API. `start()` / `stop()` / `isRunning()` control the overlay;
the four setters publish state and are safe to call from any thread.

---

## API reference

### Lifecycle

| Method | Notes |
|---|---|
| `WaylandOverlay(int screenW, int screenH)` | Size of the output you want (e.g. the captured monitor). Used to pick the matching wl_output; falls back to the largest output. `WaylandOverlay()` uses 0×0 = largest output. |
| `bool start()` | Spawns the render thread and connects to Wayland. `false` on failure (e.g. no `$WAYLAND_DISPLAY`). |
| `void stop()` | Tears down the surface and joins the thread. |
| `bool isRunning() const` | Thread-safe check. |

The class is **non-copyable and non-movable**. Setters called before `start()`
or after `stop()` are no-ops.

### Publishing state

All setters are safe to call from any thread. They swap state under a short
mutex and wake the render thread via eventfd.

```cpp
struct DetectionBox { float x, y, w, h; int label; float confidence; };
void setDetections(std::vector<DetectionBox> boxes);
```

Axis-aligned boxes, drawn as 2px opaque outlines with a per-label color and a
`"name 0.87"` label next to each box (label text from `setLabelNames`, or `?` if
unset, followed by the confidence).

```cpp
struct Shape {
    enum Kind { Filled, Line };
    Kind kind = Filled;
    float x0, y0, x1, y1;  // Filled: rect corners. Line: endpoints.
    float thickness = 1.0f; // Line only
    uint32_t color = 0xFFFFFFFF; // 0xAARRGGBB
};
void setShapes(std::vector<Shape> shapes);
```

Additive primitives (HP/shield bars, skeleton bones) that box outlines can't
express. Painted on top of the boxes.

```cpp
void setLabelNames(const std::vector<std::string>& names);
void setStats(double detectMs, int fps);
void setSourceSize(int w, int h);
```

- `setLabelNames` maps a `DetectionBox::label` to a text label.
- `setStats` renders `"det X.X ms | N fps"` top-left (fps omitted when `0`).
- `setSourceSize` is the pixel space your coordinates live in. The layer
  surface can be configured at a different size than the captured output, so
  `paint()` maps boxes/shapes by `surface / source`. `0` (default) falls back
  to the size passed to the constructor.

### Colors

Detection-box `label` indexes an 8-color palette:

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| red | green | blue | yellow | magenta | cyan | orange | purple |

Out-of-range labels wrap (`label % 8`). `Shape::color` is a raw `0xAARRGGBB`
value, not a palette index.

---

## Minimal end-to-end example

`example/main.cpp` opens the overlay on the largest output and paints boxes, a
filled bar, a line, and a stats line for ~3 s:

```sh
cmake -B build && cmake --build build -j
./build/example/overlay_example
```

Run it inside a Wayland session. Typical integration: one thread captures +
detects, then calls `setDetections()` / `setShapes()` / `setStats()` each frame;
the overlay's own render thread handles all Wayland I/O and painting.

---

## Regenerating the vendored protocol bindings

See `src/wayland/README.md`.
