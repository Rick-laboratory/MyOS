# myOS v222 - HWND paint/input/geometry state dispatch

v222 continues the USER32 state-machine rollout started in v219/v221.

## What changed

- Added private `_HWND_ACTION_GEOMETRY` and `_HWND_ACTION_HITTEST` action classes.
- LIVE HWND state now admits PAINT, CAPTURE, GEOMETRY and HITTEST as explicit action-table decisions.
- `SetCapture`/`ReleaseCapture` now enter through the `_HWND_ACTION_CAPTURE` / `_HWND_ACTION_MESSAGE` state path.
- `ScreenToClient` now enters through `_HWND_ACTION_GEOMETRY`.
- `SetWindowPos` now resolves once through `_HWND_ACTION_GEOMETRY` and reuses the resolved USER32 context.
- `ScrollWindowEx` now resolves once through `_HWND_ACTION_PAINT` before dirty/update-region work.
- GDI paint/DC validity now consumes `_HWND_ACTION_PAINT` through `hwnd_query_action()` instead of re-checking via generic `IsWindow()`.
- `ChildWindowFromPoint` now resolves the parent and candidate children through `_HWND_ACTION_HITTEST` before visual hit dispatch.

## Why

This keeps lifecycle truth in the HWND state/action table.  Boundary checks still exist, but paint/input/geometry paths no longer independently rediscover whether a HWND is live, destroying or zombie.

## Validation

- `make clean && make -j$(nproc)`
- `./myos_input --smoke user32`
- `./myos_input --smoke strict_handles`
- `./myos_input --smoke all`
