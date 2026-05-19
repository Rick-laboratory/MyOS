# myOS v122 USER32 Window Geometry Contract

v122 fixes the previous USER32 geometry gap where standalone `CreateWindowExA`
HWNDs had USER32 metadata but could not reliably answer `GetWindowRect` or be
moved through `MoveWindow` unless they also had a WindowManager/compositor slot.

## Fixed contract paths

- `GetWindowRect(hwnd, &rc)` works for standalone USER32 HWNDs.
- `MoveWindow(hwnd, x, y, w, h, repaint)` works for standalone USER32 HWNDs.
- `MoveWindow` updates the stored USER32 geometry.
- `GetWindowRect` observes the updated geometry.
- `SetWindowPos` local fallback emits:
  - `WM_WINDOWPOSCHANGING`
  - `WM_WINDOWPOSCHANGED`
  - `WM_MOVE`
  - `WM_SIZE`
- Invalid-parameter paths are covered:
  - `GetWindowRect(hwnd, NULL)` -> `ERROR_INVALID_PARAMETER`
  - `MoveWindow(NULL, ...)` -> `ERROR_INVALID_HANDLE`

## Design rule

WindowManager-backed top-level windows still use the shared WindowState path as
truth. HWNDs that are purely USER32-local use `MyWinWindowInfo::rcClient` as the
local geometry backing store. Child HWNDs keep parent-client coordinates
internally, but `GetWindowRect` converts them to screen coordinates.

## Smoke delta

The `user32` smoke group now has no WARNs and includes hard assertions for:

- initial standalone window rect
- `MoveWindow` success without WindowManager slot
- post-move rect exactness
- `WM_WINDOWPOSCHANGING` / `WM_WINDOWPOSCHANGED`
- `WM_MOVE` / `WM_SIZE`
- LastError for invalid geometry calls

Current full smoke result:

```text
SMOKE RESULT: PASS (0 failures)
```

## Remaining USER32 debt

This version intentionally does not solve the larger public message contract:

- `MSG._myos` is still a public-layout leak.
- `DispatchMessageA` still depends on private queue metadata.
- Cross-thread/cross-process `SendMessage` semantics are still not real USER32.
- Full z-order, owner/parent, activation, clipping and redraw semantics are not
  complete.

Those belong in the next USER32 message-contract pass.
