# Vendored Wayland protocol bindings

These files are **generated** and checked in so the build needs no `wayland-scanner`
or `wayland-protocols` package at compile time.

## Files

- `xdg-shell-client-protocol.h` / `xdg-shell-protocol.c` — dependency of
  layer-shell (`get_popup` references `xdg_popup`).
- `wlr-layer-shell-unstable-v1-client-protocol.h` / `wlr-layer-shell-unstable-v1-protocol.c`
  — the click-through overlay itself.
- `fractional-scale-v1-client-protocol.h` / `fractional-scale-v1-protocol.c`
  — `wp_fractional_scale_v1`, used for true (non-integer) HiDPI scale.

## Regenerating

```sh
cd src/wayland
for p in xdg-shell wlr-layer-shell-unstable-v1 fractional-scale-v1; do
    wayland-scanner client-header "$p.xml" "$p-client-protocol.h"
    wayland-scanner private-code   "$p.xml" "$p-protocol.c"
done
```

The `.xml` sources come from `/usr/share/wayland-protocols/` (or the
`wayland-protocols` package). `fractional-scale-v1.xml` is also shipped under
`/usr/share/qt6/wayland/protocols/fractional-scale/`.

### Required manual patch after regenerating

`wlr-layer-shell-unstable-v1-client-protocol.h` has a `get_layer_surface`
parameter literally named `namespace`, which is a C++ keyword and fails to
compile in our C++ translation unit. After regenerating, rename that one
parameter (and its single use in the same inline function) from `namespace`
to `ns`. The `.c` file compiles as C and needs no change.
