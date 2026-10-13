# Overlay Interactive Menu — Design Spec

Date: 2026-10-13
Status: Approved (design), pending spec review

## Goal

Add an app-driven, clickable settings panel to `WaylandOverlay`: a small menu
the app opens via API, drawn into the existing `wl_shm` surface, with mouse +
keyboard interaction. The menu exposes built-in render toggles (boxes / shapes /
stats), an opacity slider, app-registered custom settings, and a Quit button.
The app learns about changes by polling an event queue.

## Non-goals

- No overlay-owned global hotkey (the app opens the menu; the overlay never
  grabs a key while closed).
- No text input fields (hence no xkbcommon dependency).
- No enum/dropdown settings (bool toggle + float slider only in v1).
- No multiple menus, no theming, no drag/move.
- No "click outside to close" (impossible with the menu-limited input region;
  see below).

## API (new, app-facing)

```cpp
// control — safe to call from any thread
bool setMenuVisible(bool visible); // returns the resulting visibility state;
                                  // false if the overlay isn't running
void registerSetting(int id, const std::string& name, bool initial);                 // toggle
void registerSetting(int id, const std::string& name,
                     float min, float max, float step, float initial);               // slider
void setSetting(int id, float value);   // app pushes a value; slider reflects it

// poll — drain from the app's loop
struct MenuEvent {
    enum Type { None, Quit, SettingChanged };
    Type type = None;
    int settingId = -1;
    float value = 0.0f;   // bool settings report 0.0 / 1.0
};
bool pollMenuEvent(MenuEvent& out);   // true if an event was dequeued
```

### Built-in menu items

Always present, in order, above any app-registered settings:

1. **Boxes** — toggle detection-box rendering.
2. **Shapes** — toggle shape rendering.
3. **Stats** — toggle the stats line.
4. **Opacity** — slider 0–100%, global alpha multiplier on all overlay output.
5. App-registered settings (in registration order).
6. **Quit** — button; emits `MenuEvent{Quit}`.
7. **Close** — button; closes the menu (no event).

Visibility toggles and opacity are **internal**: the overlay applies them
directly and does not notify the app. Only `Quit` and `SettingChanged` reach the
app.

## Threading model

- Menu state (visible flag, item list, focus index, values) lives on the render
  thread; pointer/keyboard events arrive there via the Wayland dispatch loop.
- App-thread calls (`setMenuVisible`, `registerSetting`, `setSetting`) use the
  existing pattern: swap state under the state mutex, bump the generation
  counter, poke the eventfd.
- `pollMenuEvent` drains an internal thread-safe queue that the render thread
  pushes into (render thread never blocks on the queue; unbounded deque under a
  mutex).

## Input plumbing

- Registry handler gains a `wl_seat` case; on `capabilities`, obtain
  `wl_pointer` and `wl_keyboard` and install listeners.
- **Menu visible:** `wl_surface_set_input_region(surface, menu_rect_region)` and
  `zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 1)`.
- **Menu hidden:** empty input region and `keyboard_interactivity(0)` — the
  surface returns to fully click-through.
- **Consequence (accepted):** because the input region is limited to the menu
  rect, clicks outside the menu fall through to the game. Closing is by
  **Escape**, a **Close** button, or `setMenuVisible(false)` — never by clicking
  outside.

### Keyboard (no xkbcommon)

Raw evdev keycodes via `<linux/input-event-codes.h>`; `wl_keyboard.key` delivers
`keycode = evdev_keycode + 8`, so compare against `KEY_*` constants:

- `KEY_UP` / `KEY_DOWN` — move focus.
- `KEY_LEFT` / `KEY_RIGHT` — adjust focused slider by `step`.
- `KEY_ENTER` / `KEY_SPACE` — activate focused toggle/button.
- `KEY_ESC` — close menu.
- `KEY_TAB` — move focus (same as down).

No keysym mapping is needed (no text input), so no `libxkbcommon` dependency.

## Rendering & interaction

- Panel drawn into the same `wl_shm` buffer, top-right corner, ~320 px wide:
  translucent backing (`0xE0000000`-ish) + 1px border + title + widgets stacked
  vertically.
- Widgets:
  - **Toggle**: label + on/off box.
  - **Slider**: track + fill + thumb; drag with pointer, or ←/→ when focused.
  - **Button**: label + border.
- Focus highlight = contrasting background/border on the focused widget.
- Text: existing 5×7 font rendered at 2× integer scale for legibility.
- Any state change (hover, focus, value) triggers a repaint through the existing
  double-buffer + damage path. Mouse motion is tracked to update hover.

## Implementation outline

- `src/WaylandOverlay.h`: new public methods/structs (`MenuEvent`,
  `setMenuVisible`, `registerSetting`×2, `setSetting`, `pollMenuEvent`); private
  menu state + `wl_seat`/`wl_pointer`/`wl_keyboard` forward decls and members.
- `src/WaylandOverlay.cpp`: seat/pointer/keyboard listeners, input-region and
  keyboard-interactivity switching, menu layout + hit-testing + paint, keyboard
  focus handling, and the poll queue.
- `example/main.cpp`: register a couple of demo settings, open the menu after a
  delay, and print polled events.

## Verification

- Library + example build; example exercises `registerSetting`,
  `setMenuVisible`, and `pollMenuEvent` (compiles + links as the automated gate).
- Runtime interaction requires a live Wayland session (manual test: open menu,
  click a toggle, drag a slider, press Escape).
