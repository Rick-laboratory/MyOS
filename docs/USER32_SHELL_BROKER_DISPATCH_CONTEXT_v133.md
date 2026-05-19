# v133 USER32 Shell Broker Dispatch Context

## Problem

v131 hardened HWND access so reads like `GetWindowLongPtrA`, `GetWindowTextA`, and geometry reads require either ownership or `CAP_WINDOW_READ`. This correctly protected foreign HWNDs, but exposed a real scheduler/dispatcher bug: the compositor/render thread may pump queued messages for shell HWNDs while its ambient runtime Capability is neutral, stale, or absent.

That meant `#32769` and `Shell_TrayWnd` WndProcs could be invoked without the shell owner context. Their `GWLP_USERDATA` lookup then failed the HWND access check, so legitimate shell actions were ignored.

Observed symptoms:

- Start/right-click menu opened, but item execution did nothing.
- Desktop icon selection/open did not work through the queued DesktopWndProc path.
- Minimized windows could not be resumed from the taskbar.

## Fix

`hwnd_dispatch_message()` now enters the target window owner's runtime context before invoking the target WndProc, and restores the previous context afterward.

This models the Win32 rule we actually need: a queued message runs as work on the receiver UI thread/queue, not as arbitrary authority from the thread that happened to pump the queue.

## Security impact

This does not reopen v131 injection holes. The permission decision still happens before messages enter the queue:

- `PostMessageA` checks `mywin_can_message_window`.
- `SendMessageA`/`SendMessageTimeoutA` check `mywin_can_message_window`.
- `DispatchMessageA` still rejects forged foreign messages.
- dangerous foreign `WM_SETTEXT`, `WM_COMMAND`, `WM_MDICREATE`, `WM_MDIDESTROY`, and `WM_MDIACTIVATE` remain blocked.

The fix only ensures legitimate already-queued receiver work executes with receiver ownership.

## Smoke

New group: `shell_broker`

It verifies:

- queued Desktop `WM_COMMAND` posted by shell still executes while ambient dispatcher cap is neutral;
- DialogLab is created from that queued desktop command;
- queued taskbar click posted by shell restores a minimized app while ambient dispatcher cap is neutral.
