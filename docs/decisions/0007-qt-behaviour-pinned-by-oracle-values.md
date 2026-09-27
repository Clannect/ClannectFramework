# 0007 — Qt's math behaviour is pinned by oracle values, not by linking Qt

**Status:** accepted, 2026-09-27

## Problem

Existing Clannect scenes store rotations as Euler degrees, and interfaces are projected onto surfaces with
`QTransform::quadToQuad`. If CFW's `Quat` or `Transform2D` disagreed with Qt even slightly (a different Euler
order, a different gimbal-lock rule), every saved scene would load subtly rotated or projected after the port.

CFW must contain no Qt code, and the NoQt test enforces that. Qt cannot be linked into CFW's tests.

## Decision

- A throwaway program, outside this repository and never committed here, was built against Qt 6.8.3 (the
  version the engine ships with) and printed Qt's results for a set of inputs.
- CFW's tests (`RotationTest`, `GeometryTest`) contain only those **numbers**. They pin the behaviour; they
  contain no Qt code.
- Where Qt's behaviour was not what the textbook formula gives, the oracle decided:
  - **Gimbal lock** is detected when `1 − |sin(pitch)| ≤ 1e-5` (about 0.26° from vertical). This is Qt's
    fuzzy-zero threshold, found by bisection against the oracle.
  - **In lock**, yaw is `2·atan2(y, w)` and roll is reported as 0. The oracle's values fit this formula
    exactly, and do not fit the textbook `atan2(-2(xy − zw), …)`.

## Deliberate differences from Qt (degenerate input only)

| Case | Qt | CFW |
|---|---|---|
| Zero-area quad in `squareToQuad`/`quadToQuad` | Returns a singular matrix (everything maps to one point). | Refuses (`std::nullopt`). The engine already skips drawing when the mapping fails. |
| `toEulerDegrees` of a slightly denormalised quaternion | May return NaN (`asin` of a value just above 1). | Clamps; never NaN. |
| Normalising a zero quaternion | Returns the zero quaternion. | Returns the identity. |
| `mapRect` of a rectangle crossing a projection's vanishing line | Clips the polygon. | Bounding box of the four mapped corners (with Qt's same near-clip w). |

## Conventions that differ on purpose, and need care when porting

- `cfw::Transform2D` uses column vectors: `a * b` applies `b` first. `QTransform` uses row vectors, where
  `a * b` applies `a` first. Use `a.then(b)` when porting `QTransform` code; it reads the same way Qt's does.
- `cfw::Mat4` builds with factories (`Mat4::translation(p) * Mat4::rotation(q)`) instead of mutating in
  place (`m.translate(p); m.rotate(q)`). The composition order is the same as `QMatrix4x4`'s.

## What would change this

If a scene is found whose rotation decodes differently, add its values to the oracle tests before changing
anything. The oracle program is ~60 lines. Rebuild it against the engine's Qt, in the engine repository,
while Qt is still installed.
