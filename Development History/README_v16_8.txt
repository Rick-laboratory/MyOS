myOS v16.8 - Spy foreground/list fixes

- Spy++ Foreground button now brings the selected HWND to the foreground instead of selecting the current foreground window.
- Added SetForegroundWindow(HWND) to the WinAPI compatibility layer.
- Added wm_set_foreground_hwnd(...) in the WindowManager.
- SetWindowPos now activates the target unless SWP_NOACTIVATE is set.
- Spy++ row hit testing is aligned with the rendered table layout, so list clicks select the intended row reliably.
