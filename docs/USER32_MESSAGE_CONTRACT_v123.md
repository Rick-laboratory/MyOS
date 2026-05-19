# myOS v123 USER32 Message Contract

v123 removes the biggest public USER32 ABI leak left from the v118 audit: `MSG` no longer embeds myOS private queue transport state.

## Before v123

`sdk/include/winuser.h` exposed a private runtime payload inside the public `MSG` structure:

```c
MyMessage _myos;
```

That made `sizeof(MSG)` non-MSDN-like, forced public `winuser.h` to include `../../myqueue.h`, and meant `DispatchMessageA()` depended on private queue data being present in caller-visible memory.

The practical breakage was simple: Win32-style code that manually creates a `MSG` with only `hwnd/message/wParam/lParam` could not be trusted to dispatch like Windows.

## v123 behavior

Public `MSG` is now limited to the documented-style fields:

```c
typedef struct tagMSG {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
} MSG, *PMSG, *LPMSG;
```

Private queue metadata is preserved inside `winuser.c` using a thread-local dispatch sidecar table. `PeekMessageA()` and `GetMessageA()` convert the private `MyMessage` to public `MSG`, then store the original private message internally. `DispatchMessageA()` first tries to consume that sidecar. If no sidecar exists, it synthesizes the dispatch from the public `MSG` fields.

This keeps both important paths working:

- queued myOS messages still retain private sync/IPC metadata;
- manually constructed MSDN-shaped `MSG` values now dispatch without relying on private fields.

## Smoke coverage

The `user32` smoke group now includes:

- public `MSG` ABI-size tripwire;
- `PostMessageA` + `PeekMessageA(PM_REMOVE)` + `DispatchMessageA` queued path;
- manually constructed `MSG` + `DispatchMessageA` public-field fallback;
- the v122 `GetWindowRect`/`MoveWindow` contract checks remain green.

Current verified result:

```text
user32 summary :: checks=24 pass=24 fail=0 warn=0
SMOKE RESULT: PASS (0 failures)
```

## Still not solved here

v123 intentionally does not pretend that USER32 is complete. Remaining larger passes include:

- true cross-thread `SendMessage`/reply semantics;
- stricter thread-message queue behavior;
- desktop/thread scoped focus and capture;
- dialog manager/focus navigation cleanup;
- eventual cleanup of legacy `HWNDWndProc` void-return transport.
