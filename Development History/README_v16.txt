myOS v16.5 - Spy++ layout + Terminal wrap/scroll fixes

Build:
  make clean && make

Run:
  sudo ./myos_input <keyboard-event-device> <mouse-event-device>

New in v16.5:
  - app_spy.c / app_spy.h: first Spy++ style system tool.
  - Start menu: "Window Spy++".
  - Hotkey: F7 opens myOS Spy++.
  - WinAPI-style EnumWindows(WNDENUMPROC, LPARAM).
  - Spy lists HWND, active/minimized/subscribed flags, RECT and title.
  - Spy subscribes to WM_WINDOWPOSCHANGED for visible windows.
  - Live counter for subscription events.
  - Buttons: Refresh, Subscribe, Foreground, Rename target.
  - Rename target uses SetWindowTextA on the selected HWND.
  - Window state still flows through GetWindowRect/MyGetWindowState and queue notifications.

Notes:
  This is still PoC-level and intentionally monolithic. The important part is that the app uses public WinAPI-like routes instead of directly poking WindowManager internals.
