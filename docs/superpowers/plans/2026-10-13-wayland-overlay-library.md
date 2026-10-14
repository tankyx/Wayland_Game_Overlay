# Wayland Overlay Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the duplicated overlay class into one reusable `wayland_overlay` static library and migrate both consumers to it.

**Architecture:** A single C++ static library (`WaylandOverlay.h` + `.cpp` + vendored Wayland bindings) with zero dependencies beyond `wayland-client` and `pthread`. The two consumers link the `wayland_overlay` target via `add_subdirectory`.

**Tech Stack:** C++17, CMake ≥ 3.16, wayland-client (pkg-config), wlr-layer-shell + xdg-shell (vendored generated bindings), pthread.

**Spec:** `docs/superpowers/specs/2026-10-13-wayland-overlay-library-design.md`

## Global Constraints

- C++17 (`CMAKE_CXX_STANDARD 17`, `REQUIRED ON`).
- No X11 dependency in the library; no install rules.
- Vendored bindings are copied verbatim from the superset consumer's `src/wayland/` (already patched for the `namespace` keyword issue).
- Build type defaults to `Release` when unset (both consumers do this).

## Review Focus

| Input / condition | Expected behavior | Pinning task |
|---|---|---|
| `start()` called on a non-Wayland session (no `$WAYLAND_DISPLAY`) | `start()` returns false, app keeps running, no crash | Task 1 (existing impl already handles; build gate only) |
| `setDetections`/`setShapes` before `start()` | no-op, no crash | Task 1 (existing impl already handles) |
| Copy/assign a `WaylandOverlay` | compile error (deleted) | Task 1 |
| Consumer links without `wayland-client` pkg-config | target propagates `PkgConfig::WAYLAND` PUBLIC, so it links transitively | Task 2 build |
| The base consumer still compiles on Windows (no Linux sources) | Windows path untouched; overlay alias only changed on Linux | Task 3 |

---

### Task 1: Scaffold the `wayland_overlay` library

**Files:**
- Create: `CMakeLists.txt`
- Create: `include/WaylandOverlay.h` (the public API)
- Create: `src/WaylandOverlay.cpp` (the overlay implementation)
- Create: `src/wayland/xdg-shell-client-protocol.h`, `src/wayland/xdg-shell-protocol.c`, `src/wayland/wlr-layer-shell-unstable-v1-client-protocol.h`, `src/wayland/wlr-layer-shell-unstable-v1-protocol.c` (copied verbatim from the superset consumer)
- Create: `src/wayland/README.md` (copied from the superset consumer)
- Create: `README.md`

**Interfaces:**
- Produces: CMake target `wayland_overlay` (STATIC) with `target_include_directories(... PUBLIC include)` and `target_link_libraries(... PUBLIC PkgConfig::WAYLAND pthread)`. Class `WaylandOverlay` with methods `start()`, `stop()`, `isRunning()`, `setLabelNames(vector<string>)`, `setDetections(vector<DetectionBox>)`, `setShapes(vector<Shape>)`, `setStats(double,int)`, `setSourceSize(int,int)`; nested `DetectionBox{float x,y,w,h; int label; float confidence;}` and `Shape{enum Kind{Filled,Line}; Kind kind; float x0,y0,x1,y1; float thickness; uint32_t color;}`.

- [ ] **Step 1: Copy `src/wayland/` from the superset consumer verbatim**

Run: `mkdir -p src/wayland && cp <superset-consumer>/src/wayland/* src/wayland/`
Expected: 5 files present (`xdg-shell-client-protocol.h`, `xdg-shell-protocol.c`, `wlr-layer-shell-unstable-v1-client-protocol.h`, `wlr-layer-shell-unstable-v1-protocol.c`, `README.md`).

- [ ] **Step 2: Create `include/WaylandOverlay.h` from the superset header, renaming the class**

Create the `WaylandOverlay` header with the public API: `DetectionBox`, `Shape`, constructors, destructor, method declarations, static callback declarations, private members.

- [ ] **Step 3: Create `src/WaylandOverlay.cpp` from the superset `.cpp`, renaming the class**

Implement all `WaylandOverlay` methods (including static callbacks and `pickOutput`/`initWayland`/`teardown`/`threadMain`/`initBuffer`/`destroyBuffer`/`allocBuffers`/`maybePaint`/`paint`).

- [ ] **Step 4: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
project(wayland_game_overlay C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)

add_library(wayland_overlay STATIC
    src/WaylandOverlay.cpp
    src/wayland/xdg-shell-protocol.c
    src/wayland/wlr-layer-shell-unstable-v1-protocol.c
)
target_include_directories(wayland_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wayland_overlay PUBLIC PkgConfig::WAYLAND pthread)
```

- [ ] **Step 5: Build the library standalone to verify**

Run: `cmake -B build && cmake --build build -j`
Expected: build succeeds, produces `build/libwayland_overlay.a`.

- [ ] **Step 6: Write `README.md`** — one section: how to consume via `add_subdirectory(path)`, `target_link_libraries(app PRIVATE wayland_overlay)`; note `wayland-client` comes transitively; note FetchContent works too.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat: wayland_overlay static library extracted from the superset copy"
```

---

### Task 2: Add the standalone example

**Files:**
- Create: `example/main.cpp`
- Modify: `CMakeLists.txt` (add `add_subdirectory(example)`)

**Interfaces:**
- Consumes: `wayland_overlay` target and `WaylandOverlay` API from Task 1.
- Produces: executable `overlay_example`.

- [ ] **Step 1: Write `example/main.cpp`**

Instantiate `WaylandOverlay`, call `start()`, set label names + 2-3 `DetectionBox`es + one `Shape::Line` + one `Shape::Filled` + `setStats`, sleep ~3 s, `stop()`. Return nonzero if `start()` fails.

- [ ] **Step 2: Write `example/CMakeLists.txt`**

```cmake
add_executable(overlay_example main.cpp)
target_link_libraries(overlay_example PRIVATE wayland_overlay)
```

- [ ] **Step 3: Add `add_subdirectory(example)` to the root `CMakeLists.txt`** (guard so it's skipped when consumed as a subdirectory: `if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)`).

- [ ] **Step 4: Build the example**

Run: `cmake -B build && cmake --build build -j`
Expected: build succeeds, `build/example/overlay_example` exists.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat: standalone overlay example"
```

---

### Task 3: Migrate the base consumer

**Files:**
- Delete: `<base-consumer>/src/<old overlay header>`, `<old overlay cpp>`, `src/wayland/`
- Modify: `<base-consumer>/CMakeLists.txt` (Linux branch)
- Modify: `<base-consumer>/src/<main>.cpp` (overlay include + alias)

**Interfaces:**
- Consumes: `wayland_overlay` target from Task 1; class name `WaylandOverlay`.
- Produces: the base consumer's executable builds on Linux, unchanged on Windows.

- [ ] **Step 1: Delete the private overlay files**

Run: `rm -rf <base-consumer>/src/<old overlay header> <base-consumer>/src/<old overlay cpp> <base-consumer>/src/wayland`

- [ ] **Step 2: Edit CMakeLists Linux branch**

Remove the old overlay sources, `src/wayland/xdg-shell-protocol.c`, `src/wayland/wlr-layer-shell-unstable-v1-protocol.c` from the executable's `target_sources(...)` (keep the capture source). Add `add_subdirectory(../Wayland_Game_Overlay wayland_overlay_build)` in the `if(NOT WIN32)` block, and add `wayland_overlay` to `target_link_libraries(...)`.

- [ ] **Step 3: Edit the main source**

`#include <WaylandOverlay.h>`; alias `WaylandOverlay` as `DebugOverlay`. Leave the Windows overlay branch untouched.

- [ ] **Step 4: Build on Linux**

Run: `cmake -B build && cmake --build build -j` (in `<base-consumer>`)
Expected: build succeeds, the executable links against `wayland_overlay`.

- [ ] **Step 5: Commit**

```bash
git -C <base-consumer> add -A
git -C <base-consumer> commit -m "refactor: use shared wayland_overlay library"
```

---

### Task 4: Migrate the superset consumer

**Files:**
- Delete: `<superset-consumer>/src/<old overlay header>`, `<old overlay cpp>`, `src/wayland/`
- Modify: `<superset-consumer>/CMakeLists.txt`
- Modify: `<superset-consumer>/src/<main>.cpp` (overlay include and class references)

**Interfaces:**
- Consumes: `wayland_overlay` target, `WaylandOverlay`, `WaylandOverlay::Shape`.
- Produces: the superset consumer's executable builds; its app-specific window-detection header + `HAVE_X11` retained.

- [ ] **Step 1: Delete the private overlay files**

Run: `rm -rf <superset-consumer>/src/<old overlay header> <superset-consumer>/src/<old overlay cpp> <superset-consumer>/src/wayland`

- [ ] **Step 2: Edit the CMakeLists**

Remove the old overlay sources, `src/wayland/xdg-shell-protocol.c`, `src/wayland/wlr-layer-shell-unstable-v1-protocol.c` from `add_executable(...)` (keep the main source, its memory-reader header, and the window-detection header). Add `add_subdirectory(../Wayland_Game_Overlay wayland_overlay_build)` and add `wayland_overlay` to `target_link_libraries(...)`. Keep the `HAVE_X11` block as-is.

- [ ] **Step 3: Edit the main source**

`#include <WaylandOverlay.h>`; use the `WaylandOverlay` class (including `WaylandOverlay::Shape`).

- [ ] **Step 4: Build**

Run: `cmake -B build && cmake --build build -j` (in `<superset-consumer>`)
Expected: build succeeds, the executable links against `wayland_overlay`.

- [ ] **Step 5: Commit**

```bash
git -C <superset-consumer> add -A
git -C <superset-consumer> commit -m "refactor: use shared wayland_overlay library"
```
