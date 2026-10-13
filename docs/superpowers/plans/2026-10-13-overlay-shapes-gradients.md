# Overlay Shapes & Gradients Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `Ellipse`/`Triangle`/`Polygon` shape kinds and linear (H/V) gradients to `WaylandOverlay::Shape`, painted in the existing software rasterizer.

**Architecture:** New pure rasterizer helpers live in a header-only `src/raster.h` (no Wayland dependency), unit-tested standalone; `WaylandOverlay.cpp` includes them and dispatches in its existing scene-bounds and paint passes.

**Tech Stack:** C++17, header-only inline rasterizer, assert-based test executable (no framework).

**Spec:** `docs/superpowers/specs/2026-10-13-overlay-shapes-gradients-design.md`

## Global Constraints

- `Shape` change is append-only: existing `{Filled, x0,y0,x1,y1,thickness,color}` and `{Line, …}` aggregate inits must keep compiling.
- New fields appended after `color`: `uint32_t color2`, `Gradient gradient`, `std::vector<float> pts`.
- `enum Kind { Filled, Line, Ellipse, Triangle, Polygon }`, `enum Gradient { None, Horizontal, Vertical }`.
- Ellipse = fill only; Triangle/Polygon = solid only; gradient on Filled rect + Ellipse only.
- No new dependencies; C++17.

## Review Focus

| Input / condition | Expected behavior | Pinning task |
|---|---|---|
| `Shape{Filled,…}` with 7 init args (Halo's usage) | still compiles | Task 3 (build Halo) |
| Gradient with `gradient == None` or `color == color2` | solid fill, no divide-by-zero | Task 1 test |
| Ellipse with zero/negative radius | clamped to 1px, no hang | Task 1 test |
| Triangle/Polygon with < 3 points | ignored | Task 1 test |
| Horizontal gradient endpoints | leftmost pixel == `color`, rightmost == `color2` | Task 1 test |

---

### Task 1: Rasterizer helpers + self-test

**Files:**
- Create: `src/raster.h`
- Create: `tests/raster_test.cpp`
- Modify: `CMakeLists.txt` (add test executable, guarded so it's skipped when consumed as a subdirectory)

**Interfaces:**
- Produces (inline, in `src/raster.h`): `uint32_t lerpColor(uint32_t a, uint32_t b, float t)`; `void fillRectGradient(uint32_t* px, int surfW, int surfH, int x, int y, int w, int h, uint32_t c0, uint32_t c1, int axis)`; `void fillEllipse(uint32_t* px, int surfW, int surfH, float cx, float cy, float rx, float ry, uint32_t color)`; `void fillEllipseGradient(uint32_t* px, int surfW, int surfH, float cx, float cy, float rx, float ry, uint32_t c0, uint32_t c1, int axis)`; `void fillPolygon(uint32_t* px, int surfW, int surfH, const std::vector<float>& pts, uint32_t color)`. `axis`: 0=horizontal, 1=vertical. All clip to `[0,surfW)×[0,surfH)`. `lerpColor` = per-channel `lroundf(a + (b-a)*t)`.
- Consumes: nothing.

- [ ] **Step 1: Write the failing test `tests/raster_test.cpp`**

A `main()` that `#include "raster.h"`, fills a 32×32 zeroed buffer, and asserts (exit nonzero on failure):
1. `lerpColor(0xFF000000, 0xFFFFFFFF, 0.0f) == 0xFF000000` and `lerpColor(..., 1.0f) == 0xFFFFFFFF`.
2. Horizontal gradient rect `(0,0,32,32)` with `c0=0xFFFF0000`, `c1=0xFF0000FF`, axis 0: pixel `(0,16) == c0`, pixel `(31,16) == c1`, pixel `(16,16)` has red>0 and blue>0.
3. Gradient with `c0 == c1` fills solid (pixel `(16,16) == c0`).
4. `fillEllipse` center `(16,16)` rx=ry=8: pixel `(16,16)` set, corner `(1,1)` still 0; radius 0 → no crash, at least center pixel set (clamp to 1px).
5. `fillPolygon` triangle `(0,0),(31,0),(15,31)`: pixel `(15,5)` set (inside), pixel `(30,30)` still 0 (outside). A 2-point polygon is ignored (buffer unchanged).

- [ ] **Step 2: Run it to confirm it fails**

Run: `cmake -B build && cmake --build build --target raster_test -j && ./build/tests/raster_test`
Expected: compile FAIL (`raster.h` not found).

- [ ] **Step 3: Implement `src/raster.h`**

Header-only inline functions with the signatures above. `fillRectGradient`/`fillEllipseGradient` lerp `c0`→`c1` per pixel: horizontal = `(x)/(w)` fraction across the rect, vertical = `(y)/(h)`; guard `w<=0`/`h<=0` → solid `c0`. `fillEllipse` = scanline fill where `((x-cx)/rx)^2 + ((y-cy)/ry)^2 <= 1`. `fillPolygon` = even-odd rule: for each row, collect edge x-intersections, sort, fill spans.

- [ ] **Step 4: Run the test to confirm it passes**

Run: same command as Step 2.
Expected: PASS (exit 0).

- [ ] **Step 5: Commit**

```bash
git add src/raster.h tests/raster_test.cpp CMakeLists.txt
git commit -m "feat: rasterizer helpers (gradient, ellipse, polygon) with self-test"
```

---

### Task 2: Wire new kinds into the paint path

**Files:**
- Modify: `include/WaylandOverlay.h` (extend `Shape`)
- Modify: `src/WaylandOverlay.cpp` (include `raster.h`, dispatch)

**Interfaces:**
- Consumes: raster helpers from Task 1.
- Produces: `Shape` with `Kind {Filled, Line, Ellipse, Triangle, Polygon}`, `Gradient {None, Horizontal, Vertical}`, fields `uint32_t color2`, `Gradient gradient`, `std::vector<float> pts`. `paint()` renders each new kind.

- [ ] **Step 1: Extend `Shape` in the header**

Append to `Shape`: `enum Kind { Filled, Line, Ellipse, Triangle, Polygon };` (replacing the current `enum Kind { Filled, Line };`), `enum Gradient { None, Horizontal, Vertical };`, and after `uint32_t color`: `uint32_t color2 = 0xFFFFFFFF; Gradient gradient = None; std::vector<float> pts;`. Keep existing fields and their order/names unchanged.

- [ ] **Step 2: Dispatch in `WaylandOverlay.cpp`**

`#include "raster.h"` (after the wayland includes). In the shape scene-bounds loop, add the point extents: for `Ellipse` use `(x0-x1, y0-y1)-(x0+x1, y0+y1)`; for `Triangle`/`Polygon` union the min/max of `pts` (after mapping by `sx`/`sy`). In the paint loop, replace the `Line ? drawLine : fillRect` dispatch with a switch: `Line`→`drawLine`, `Filled`/`Ellipse`→gradient or solid fill (`gradient==None || color==color2` → solid), `Triangle`/`Polygon`→`fillPolygon` (solid). Map `pts` by `sx`/`sy` in the same place `x0..y1` are mapped.

- [ ] **Step 3: Build the library**

Run: `cmake -B build && cmake --build build -j`
Expected: library + example build.

- [ ] **Step 4: Commit**

```bash
git add include/WaylandOverlay.h src/WaylandOverlay.cpp
git commit -m "feat: Ellipse/Triangle/Polygon shapes and linear gradients"
```

---

### Task 3: Example + backward-compat verification

**Files:**
- Modify: `example/main.cpp`
- Verify: `Halo-MCC` still builds (pins the `Shape{Filled,…}` 7-arg aggregate init)

**Interfaces:**
- Consumes: `Shape` API from Task 2.

- [ ] **Step 1: Extend `example/main.cpp`**

Add to the `shapes` vector one `Ellipse` (gradient), one `Triangle` (via `pts`), one `Polygon` (via `pts`), and one `Filled` rect with a horizontal gradient — so all new paint paths run in the demo.

- [ ] **Step 2: Build the example**

Run: `cmake -B build && cmake --build build -j`
Expected: `build/example/overlay_example` builds.

- [ ] **Step 3: Build Halo-MCC (backward-compat gate)**

Run: `cmake --build build -j` (in `Halo-MCC`)
Expected: `halo3_overlay` still builds — confirms the 7-arg `Shape{Filled,…}` aggregate init is unbroken.

- [ ] **Step 4: Commit**

```bash
git add example/main.cpp
git commit -m "docs: exercise new shapes and gradients in the example"
```
