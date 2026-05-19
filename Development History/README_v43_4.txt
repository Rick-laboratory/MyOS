myOS v43.4 - USER32 WM_NCCREATE / immortal close-slot fix

Root cause fixed:
- Some WinAPI-style app WndProcs returned 0 for WM_NCCREATE after the refactor.
- CreateWindowExA correctly treats FALSE from WM_NCCREATE as creation failure.
- The app HWND was destroyed immediately, but the desktop WindowManager slot still
  became visible with app_hwnd=0.
- Clicking X later had no HWND to send WM_CLOSE to, so Calculator/Editor/ControlLab/
  DragLab-style windows could become visually immortal.

Fixes:
- Calculator and Editor WndProc wrappers now route WM_NCCREATE/default messages to
  DefWindowProcA, while still handling WM_CREATE/input locally.
- ControlLab and DragLab top-level WndProcs now use DefWindowProcA for default
  messages instead of returning 0 for WM_NCCREATE.
- ControlLab child control proc accepts WM_NCCREATE for future CreateWindowExA use.
- WindowManager refuses to finish an app shell window if app_hwnd creation failed.
- Shell close fallback now closes broken visible slots even when no HWND exists.

Smoke test targets:
1. Startup calculator closes via X.
2. F3 calculators close via X after reopening multiple times.
3. ClipMenuLab still closes via X.
4. Editor, ControlLab, PaintLab, DragLab close via X.
5. Reopen closed apps and verify taskbar/focus do not keep tombstone windows alive.
