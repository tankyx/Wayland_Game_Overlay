# Overlay Shapes & Gradients — Design Spec

Date: 2026-10-13
Status: Approved (design), pending spec review

## Goal

Extend the `WaylandOverlay::Shape` primitive with additional filled shapes and
linear gradients, so overlays can draw circles, triangles, arbitrary polygons,
and gradient-filled bars (HP/shield bars) in the same software paint path.

## Non-goals

- No outline/stroke ellipse (fill only in v1).
- No gradient on `Triangle`/`Polygon` (solid color only in v1).
- No radial gradients (linear horizontal + vertical only in v1).
- No text/image primitives.
- No new Wayland surfaces or dependencies.

## API change

`WaylandOverlay::Shape` gains fields (appended, so existing aggregate
initializations stay source-compatible):

```cpp
struct Shape {
    enum Kind { Filled, Line, Ellipse, Triangle, Polygon };
    enum Gradient { None, Horizontal, Vertical };
    Kind kind = Filled;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Filled: corners; Line: endpoints; Ellipse: center + rx,ry
    float thickness = 1.0f;               // Line only
    uint32_t color  = 0xFFFFFFFF;         // 0xAARRGGBB
    uint32_t color2 = 0xFFFFFFFF;         // gradient end color
    Gradient gradient = None;
    std::vector<float> pts;               // Triangle(6) / Polygon(2N): x,y pairs
};
```

Existing uses — `{Shape::Filled, x0, y0, x1, y1, thickness, color}` and
`{Shape::Line, x0, y0, x1, y1, thickness, color}` — remain valid because new
members are appended and value-initialized.

## Semantics

### Shapes

| Kind | Geometry | Fill | Notes |
|---|---|---|---|
| `Filled` | rect `(x0,y0)-(x1,y1)` | solid or gradient | unchanged |
| `Line` | segment `(x0,y0)-(x1,y1)` | — | unchanged |
| `Ellipse` | center `(x0,y0)`, radii `(x1,y1)` | solid or gradient | filled only |
| `Triangle` | 3 points in `pts` | solid | |
| `Polygon` | N points in `pts` | solid | |

- `Ellipse` radii are absolute (not diameters); negative/zero radii clamp to 1px.
- `Triangle`/`Polygon` with fewer than 3 points are ignored.
- Non-finite coordinates are skipped (matches existing box handling).

### Gradients

- Applies to `Filled` rects and `Ellipse` only.
- `Horizontal`: color lerped along each scanline from `color` (left) to `color2`
  (right), per-pixel.
- `Vertical`: color lerped along each row from `color` (top) to `color2`
  (bottom).
- `None` (or `color == color2`): solid `color`.
- Lerp is per-channel 8-bit linear interpolation on the 0xAARRGGBB value
  (alpha included).

## Implementation

- Add a `fillRectGradient(...)` and `fillEllipse(...)`/`fillEllipseGradient(...)`
  helper next to the existing `fillRect`/`drawLine` in `src/WaylandOverlay.cpp`
  (anonymous namespace).
- Extend the scene-bounds pass to include `Triangle`/`Polygon` point extents and
  `Ellipse` extents, so damage tracking covers them.
- Extend the paint pass to dispatch on `kind` and apply gradients.
- `Shape` construction sites in consumers (YOLO, Halo) need no changes.

## Verification

- Library + example still build; example extended to draw one of each new kind
  (ellipse, triangle, polygon, gradient rect) so the paint path is exercised.
- Runtime rendering requires a live Wayland session; compile+link is the
  automated gate.
