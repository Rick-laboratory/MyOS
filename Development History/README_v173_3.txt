myOS / Linux-Win32
BUILD: myos_v173_4_oop_nc_resize_frame_fix

Fixes the remaining OOP DialogLab live-resize artifact after v173.4.

Root cause:
- The shell frame resizes synchronously during mouse drag.
- The OOP child receives WM_SIZE asynchronously and republishes its retained GDI stream one or more frames later.
- Until that new child frame arrives, newly exposed client pixels must not show the desktop.

Fix:
- APP_IPC_PROXY client area is compositor-filled to the current live client rectangle before drawing retained child GDI/surface content.
- Retained OOP GDI FILLRECT/RECTANGLE commands are clipped to the current client rectangle, so stale pre-resize streams cannot overdraw outside a shrunken frame.
- DialogLab's v173.4/v173.4 per-HWND owner geometry and WM_SIZE repaint path remain intact.

This is the compositor fallback erase path for OOP HWNDs, not a DialogLab special case.
