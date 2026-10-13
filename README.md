# Wayland Game Overlay

A reusable, dependency-light C++ class for drawing a click-through debug
overlay on Wayland — the kind of overlay you put over a game to draw detection
boxes, HP bars, skeletons, or stats.

- Fullscreen wlr-layer-shell surface on the **overlay** layer (KWin + wlroots),
  so it floats above the game and never steals input.
- Software-painted `wl_shm` ARGB8888 buffers — no cairo, no EGL, no GPU.
- Thread-safe: a dedicated render thread owns the Wayland connection; setters
  are called from your detection thread.
- Only runtime dependency is `wayland-client` (+ `pthread`). The wlr-layer-shell
  and xdg-shell protocol bindings are vendored and compiled in.

## Usage

```cpp
#include <WaylandOverlay.h>

WaylandOverlay overlay(1920, 1080); // preferred output size (picks the monitor)
if (!overlay.start()) return 1;

overlay.setLabelNames({"enemy", "friend"});
overlay.setDetections({{100, 200, 50, 100, 0, 0.92f}});
overlay.setShapes({{WaylandOverlay::Shape::Filled, 10, 10, 200, 30, 0, 0xFF303030}});
overlay.setStats(3.2, 60);
overlay.setSourceSize(1920, 1080); // space the boxes are expressed in

// ... run your loop ...
overlay.stop();
```

The class is non-copyable and non-movable. Setters are safe to call from any
thread. `start()` returns `false` if there is no Wayland session.

## Consuming in your CMake project

```cmake
add_subdirectory(path/to/Wayland_Game_Overlay wayland_overlay_build)
target_link_libraries(your_app PRIVATE wayland_overlay)
```

`wayland-client` is linked transitively, so you don't need to find it yourself.
`FetchContent` works the same way (the target name is `wayland_overlay`).

## Building the example

```sh
cmake -B build && cmake --build build -j
./build/example/overlay_example
```

Run the example inside a Wayland session; it paints a few boxes, a line, a
filled rect, and a stats line for a few seconds.

## Regenerating the vendored protocol bindings

See `src/wayland/README.md`.
