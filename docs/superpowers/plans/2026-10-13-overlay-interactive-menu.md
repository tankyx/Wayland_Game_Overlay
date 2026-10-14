# Overlay Interactive Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an app-driven, mouse+keyboard settings panel to `WaylandOverlay` with built-in render toggles, an opacity slider, app-registered settings, a Quit button, and a pollable event queue.

**Architecture:** Pure menu helpers live in `src/menu.h` (unit-tested). The overlay binds `wl_seat`/`wl_pointer`/`wl_keyboard`, swaps the input region between empty (click-through) and the menu rect, and draws the panel into the existing `wl_shm` buffer. App ↔ overlay flows are: setters (mutex + generation + eventfd, like existing) and `pollMenuEvent` (a mutex-guarded deque).

**Tech Stack:** C++17, wayland-client (seat/pointer/keyboard), wlr-layer-shell (on-demand keyboard interactivity = 2), `<linux/input-event-codes.h>` (raw evdev keycodes, no xkbcommon).

**Spec:** `docs/superpowers/specs/2026-10-13-overlay-interactive-menu-design.md`

## Global Constraints

- Keyboard interactivity: `ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND` (value **2**) when menu visible, `NONE` (0) when hidden.
- Input region: empty when hidden, menu rect when visible.
- Keycodes via `keycode - 8` = evdev; map `KEY_UP/DOWN/LEFT/RIGHT/ENTER/SPACE/ESC/TAB` from `<linux/input-event-codes.h>`.
- `bool` settings report as `float` 0.0/1.0 in `MenuEvent.value`.
- Built-in ids: `-1` Boxes, `-2` Shapes, `-3` Stats, `-4` Opacity, `-5` Quit, `-6` Close. App settings use `id >= 0`.
- No new dependencies; C++17.

## Review Focus

| Input / condition | Expected behavior | Pinning task |
|---|---|---|
| `setMenuVisible(true)` before `start()` | returns false, no crash | Task 2 (guard) |
| Slider value outside `[min,max]` or not on `step` | clamped + snapped | Task 1 test |
| Keycode from a non-menu key | ignored (no crash) | Task 1 test |
| `pollMenuEvent` on empty queue | returns false, leaves `out` untouched | Task 2 |
| Two settings with the same `id` | last registration wins (replace) | Task 2 |

---

### Task 1: Pure menu helpers + self-test

**Files:**
- Create: `src/menu.h`
- Create: `tests/menu_test.cpp`
- Modify: `CMakeLists.txt` (add `menu_test` executable + `add_test`)

**Interfaces:**
- Produces: `struct MenuItem { enum Type { Toggle, Slider, Button }; Type type = Toggle; int id = -1; std::string name; float min = 0, max = 1, step = 1, value = 0; };` and `enum class MenuKey { None, Up, Down, Left, Right, Activate, Close };`, `MenuKey menuKeyFor(uint32_t evdevKey)`, `float clampStep(float v, float min, float max, float step)`.

- [ ] **Step 1: Write the failing test `tests/menu_test.cpp`**

`main()` that `#include "menu.h"` and asserts (nonzero exit on failure):
1. `menuKeyFor(KEY_UP) == MenuKey::Up`, `KEY_DOWN → Down`, `KEY_LEFT → Left`, `KEY_RIGHT → Right`, `KEY_ESC → Close`, `KEY_ENTER → Activate`, `KEY_SPACE → Activate`, `KEY_TAB → Down`, and `KEY_A → None`.
2. `clampStep(0.5f, 0, 1, 0.1f) == 0.5f`, `clampStep(0.23f, 0, 1, 0.1f) == 0.2f`, `clampStep(1.5f, 0, 1, 0.1f) == 1.0f`, `clampStep(-5.f, 0, 10, 1.f) == 0.f`.

- [ ] **Step 2: Run it to confirm it fails**

Run: `cmake -B build && cmake --build build --target menu_test -j && ./build/menu_test`
Expected: compile FAIL (`menu.h` not found).

- [ ] **Step 3: Implement `src/menu.h`**

`#include <linux/input-event-codes.h>`. `menuKeyFor` switches on the evdev key. `clampStep` clamps to `[min,max]`, then `min + roundf((v-min)/step)*step` when `step > 0`, then re-clamps.

- [ ] **Step 4: Run the test to confirm it passes**

Expected: exit 0, "menu self-check: all checks passed".

- [ ] **Step 5: Commit**

```bash
git add src/menu.h tests/menu_test.cpp CMakeLists.txt
git commit -m "feat: menu helpers (key map, slider clamp) with self-test"
```

---

### Task 2: Public API + state + app-thread impls

**Files:**
- Modify: `include/WaylandOverlay.h`
- Modify: `src/WaylandOverlay.cpp`

**Interfaces:**
- Produces (public): `struct MenuEvent { enum Type { None, Quit, SettingChanged }; Type type = None; int settingId = -1; float value = 0.0f; };`, `bool setMenuVisible(bool)`, `void registerSetting(int, const std::string&, bool)`, `void registerSetting(int, const std::string&, float, float, float, float)`, `void setSetting(int, float)`, `bool pollMenuEvent(MenuEvent&)`.
- Consumes: `MenuItem` from Task 1.

- [ ] **Step 1: Extend the header**

Add forward decls `struct wl_seat; struct wl_pointer; struct wl_keyboard;`. Add the public API above. Add private render-thread fields: `bool _menuVisible=false; int _menuFocus=0, _menuHover=-1; bool _pointerDrag=false; std::vector<MenuItem> _settings, _menuRows; bool _showBoxes=true, _showShapes=true, _showStats=true; float _opacity=1.0f; int _menuX=0,_menuY=0,_menuW=0,_menuH=0; int _ptrX=0,_ptrY=0; wl_seat *_seat=nullptr; wl_pointer *_pointer=nullptr; wl_keyboard *_keyboard=nullptr;`. Add shared fields guarded by `_stateMutex`: `bool _sharedMenuVisible=false; std::vector<MenuItem> _sharedSettings;` and `std::deque<MenuEvent> _events; std::mutex _eventMutex;` (poll queue). Add `#include <deque>` and `#include "menu.h"`.

- [ ] **Step 2: Implement the four setters + `pollMenuEvent` in the `.cpp`**

`registerSetting`/`setSetting`: build/replace a `MenuItem` in `_sharedSettings` keyed by `id` (bool → `Toggle`, `min=0,max=1,step=1,value=0/1`; float → `Slider` with given min/max/step/initial), then `_sharedGen.fetch_add(1, release)` + poke `_wakeFd`. `setMenuVisible`: guard `if (!_running) return false;` then set `_sharedMenuVisible`, bump gen, poke; return the value. `pollMenuEvent`: lock `_eventMutex`, pop-front into `out`, return whether one was popped.

- [ ] **Step 3: Build**

Run: `cmake -B build && cmake --build build -j` (with `TMPDIR=/home/tanguy/.cache/build-tmp`).
Expected: library + example + `menu_test` build.

- [ ] **Step 4: Commit**

```bash
git add include/WaylandOverlay.h src/WaylandOverlay.cpp
git commit -m "feat: overlay menu public API and state"
```

---

### Task 3: Seat / pointer / keyboard bindings

**Files:**
- Modify: `include/WaylandOverlay.h` (static callback decls)
- Modify: `src/WaylandOverlay.cpp`

**Interfaces:**
- Consumes: `wl_seat`, `wl_pointer`, `wl_keyboard` members from Task 2.
- Produces: `_pointer`/`_keyboard`/`_seat` bound; `_ptrX/_ptrY` updated on motion/enter.

- [ ] **Step 1: Bind `wl_seat` in `registryGlobal`**

Add a `wl_seat_interface.name` branch: `_seat = wl_registry_bind(reg, name, &wl_seat_interface, min(version,7u))`; install `wl_seat_listener { seatCapabilities, seatName }` (name = no-op stub).

- [ ] **Step 2: Implement `seatCapabilities` + pointer/keyboard listeners**

`seatCapabilities`: on `WL_SEAT_CAPABILITY_POINTER` (and `!_pointer`) → `wl_seat_get_pointer` + `wl_pointer_listener { enter, leave, motion, button, axis(no-op) }`; on `KEYBOARD` (and `!_keyboard`) → `wl_seat_get_keyboard` + `wl_keyboard_listener { keymap(no-op), enter(no-op), leave(no-op), key, modifiers(no-op) }`. `pointerEnter`/`pointerMotion`: set `_ptrX/_ptrY` (stub interaction until Task 5); `button`/`key`: no-op stubs for now.

- [ ] **Step 3: Destroy in `teardown()`**

`wl_keyboard_destroy`/`wl_pointer_destroy`/`wl_seat_destroy` (guarded non-null), null them out.

- [ ] **Step 4: Build + commit**

```bash
cmake --build build -j
git add -A && git commit -m "feat: bind wl_seat/pointer/keyboard for menu input"
```

---

### Task 4: Menu model, layout, rendering, input region

**Files:**
- Modify: `src/WaylandOverlay.cpp` (+ private method decls in header)

**Interfaces:**
- Produces (private): `void rebuildRows()`, `void layoutMenu()`, `void applyInputRegion()`, `int rowAt(int x,int y) const`, and a `dim(uint32_t color, float alpha)` free helper in the .cpp.

- [ ] **Step 1: `rebuildRows()` + `layoutMenu()`**

`rebuildRows()` builds `_menuRows` = [Boxes toggle(`-1`, value `_showBoxes`), Shapes(`-2`), Stats(`-3`), Opacity slider(`-4`, min 0 max 100 step 1, value `_opacity*100`)] + `_settings` (app settings) + [Quit button(`-5`), Close button(`-6`)]; clamp `_menuFocus` to the new size. `layoutMenu()` computes panel rect: width 320, margin 16, row height 28, title 22, padding 8 → `_menuX=_width-320-16`, `_menuY=16`, `_menuW=320`, `_menuH=8+22+rows*28+8`. Constants local to the .cpp.

- [ ] **Step 2: `applyInputRegion()` + sync in `maybePaint()`**

`applyInputRegion()`: if `_menuVisible`, `wl_surface_set_input_region(_surface, region over _menuX/Y/W/H)` + `set_keyboard_interactivity(_layerSurface, ON_DEMAND)`; else empty region + `NONE`. In `maybePaint()`, when the generation changes, also copy `_sharedSettings → _settings` and `_sharedMenuVisible → _menuVisible`; if visibility changed → `rebuildRows()`, `layoutMenu()`, `applyInputRegion()`, and set `_scenePending`.

- [ ] **Step 3: Render the menu in `paint()`**

When `_menuVisible`: add `IntRect{_menuX,_menuY,_menuX+_menuW,_menuY+_menuH}` to `scene`; draw panel backing (`0xE0000000`) + 1px border + title "Overlay" + each row (label via `drawText`, toggle on/off box, slider track/fill/thumb, button border), focus row highlighted (lighter backing), hover row subtle. Menu drawn with full colors (no `dim`).

- [ ] **Step 4: Apply built-in toggles + opacity to overlay content**

Add `dim()` helper. In `paint()`: gate stats drawing + stats `scene` rect on `_showStats`; gate box loop on `_showBoxes`; gate shape loop on `_showShapes`; wrap each overlay-content color in `dim(color, _opacity)`. Menu is not dimmed.

- [ ] **Step 5: Build + commit**

```bash
cmake --build build -j
git add -A && git commit -m "feat: menu layout, rendering, and input-region switching"
```

---

### Task 5: Interaction + event queue

**Files:**
- Modify: `src/WaylandOverlay.cpp`

**Interfaces:**
- Consumes: `_menuRows`, `rowAt`, `_menuFocus/_menuHover/_pointerDrag`, `_events`/`_eventMutex`.
- Produces: `pollMenuEvent` returns events; interaction mutates rows and enqueues.

- [ ] **Step 1: Pointer interaction**

`pointerEnter/Leave`: set/clear `_menuHover` (via `rowAt`). `pointerMotion`: update `_ptrX/_ptrY`; if `_pointerDrag`, set the focused slider's value from x (map thumb position → value via `clampStep`); else update `_menuHover`. `pointerButton` (left, `WL_POINTER_BUTTON_STATE_PRESSED`): if over a row, focus it and activate — Toggle flips `value`; Button: `-5`→enqueue `Quit`, `-6`→close menu (set `_menuVisible=false`, `applyInputRegion`); Slider → set value from x, `_pointerDrag=true`. On release: `_pointerDrag=false`. Every mutation sets `_scenePending=true`.

- [ ] **Step 2: Keyboard interaction**

`keyboardKey` (state `WL_KEYBOARD_KEY_STATE_PRESSED`): `MenuKey k = menuKeyFor(key-8)`; `Up/Down` → `_menuFocus = clamp(_menuFocus ± 1)`, `_menuHover=-1`; `Left/Right` → focused slider value `clampStep(value ∓ step)`; `Activate` → activate focused row (same as click); `Close` → close menu. Enqueue `SettingChanged{id,value}` whenever an app-setting (`id>=0`) value changes; sync built-in ids back to `_showBoxes/_showShapes/_showStats/_opacity`. `_scenePending=true`.

- [ ] **Step 3: `enqueueEvent` helper + wire `pollMenuEvent`**

`enqueueEvent(MenuEvent e)`: lock `_eventMutex`, push to `_events`. (Used by Steps 1-2; `pollMenuEvent` from Task 2 already drains it.)

- [ ] **Step 4: Build + commit**

```bash
cmake --build build -j
git add -A && git commit -m "feat: menu mouse/keyboard interaction and events"
```

---

### Task 6: Example + full gate

**Files:**
- Modify: `example/main.cpp`

- [ ] **Step 1: Exercise the menu in `example/main.cpp`**

After `start()`: `registerSetting(0, "Confidence", 0.0f, 1.0f, 0.05f, 0.5f)` and `registerSetting(1, "ESP", true)`; open with `setMenuVisible(true)` after a short delay; in a loop, `while (pollMenuEvent(ev))` print `Quit` / `SettingChanged{id,value}`; after ~3 s, `setMenuVisible(false)` and `stop()`.

- [ ] **Step 2: Build the library + example + both tests**

Run: `cmake -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: builds; `raster_test` and `menu_test` pass.

- [ ] **Step 3: Backward-compat gates — build the consumers**

Run: `cmake --build build -j` in each consumer repo (with `TMPDIR`).
Expected: both consumers still build (public API is additive).

- [ ] **Step 4: Commit**

```bash
git add example/main.cpp
git commit -m "docs: exercise the interactive menu in the example"
```

---

## Manual runtime checklist (requires a live Wayland session)

Run `./build/example/overlay_example`: after ~1 s the panel appears top-right. Click a toggle, drag the Confidence slider, press ↑/↓/Enter/Escape. Confirm Escape or Close dismisses the menu and the overlay returns to click-through, and that printed events match the interactions.
