myOS v43.2 - USER32 close/shell sync fix

Fixes a v43/v43.1 regression where USER32-backed apps such as Calculator
and ControlLab could accept WM_CLOSE, destroy their HWND through DefWindowProcA,
but leave the desktop WindowManager frame visually alive.

The shell close path now mirrors the result after sending WM_CLOSE:
  - if the target HWND is gone after WM_CLOSE, mark the WindowManager slot closed
  - if the HWND still exists, treat that as a real app-side cancel/consume
  - if sending fails, keep the old legacy fallback

This preserves the Win32-like first-refusal model while keeping shell state in sync.
