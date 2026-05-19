# myOS v124 USER32 Lifecycle / Message-Order Contract

v124 hardens the next USER32 layer after the v122 geometry pass and v123 public `MSG` pass.

This version is intentionally not a controls/dialog polish pass. It establishes tripwires for the lifecycle and queue semantics that controls, dialogs, menus and legacy apps depend on.

## Core changes

### `CreateWindowExA` lifecycle

A visible window creation path is now smoke-gated for this order:

```text
WM_NCCREATE
WM_CREATE
WM_SHOWWINDOW
```

`CREATESTRUCTA` is also checked for coordinates, size and `lpCreateParams` preservation.

### Create aborts

v124 smoke-gates two abort paths:

```text
WM_NCCREATE returns FALSE -> CreateWindowExA returns NULL
WM_CREATE returns -1      -> CreateWindowExA returns NULL and tears down the partial HWND
```

The second path required a real code fix: before v124, `WM_CREATE`'s return value was ignored.

### Destroy lifecycle

Destroy is smoke-gated for this order:

```text
WM_DESTROY
WM_NCDESTROY
```

The HWND must no longer be valid after the destroy/unlink path completes.

### `WM_CLOSE` contract

v124 distinguishes delivery from default policy:

```text
SendMessage(hwnd, WM_CLOSE, ...) reaches the app WndProc.
A custom WndProc can ignore it and keep the HWND alive.
DefWindowProcA(WM_CLOSE) calls DestroyWindow.
```

This matters because many real Win32 apps use `WM_CLOSE` as a confirmation/cleanup decision point.

### Queue semantics

v124 adds tripwires for:

```text
PostMessage FIFO order for normal-priority messages
PeekMessageA(PM_NOREMOVE) not removing
PeekMessageA(PM_REMOVE) removing
DispatchMessageA of queued public MSG values
GetMessageA returning FALSE for WM_QUIT
PostQuitMessage exit code preservation in MSG.wParam
```

## Public SDK fix

`PostQuitMessage` is now declared in `sdk/include/winuser.h`.

## Important lpParam fix

Before v124, `CreateWindowExA` treated non-NULL `lpParam` as if it might be an internal myOS thunk and dereferenced it before creating the HWND.

That is not Win32-compatible. `lpParam` belongs to the application and must be passed through `CREATESTRUCTA::lpCreateParams`. v124 removes that public-API dereference and keeps internal thunk state separate.

## Smoke result

```text
lifecycle summary :: checks=32 pass=32 fail=0 warn=0
SMOKE RESULT: PASS (0 failures)
```
