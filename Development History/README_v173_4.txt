myOS / Linux-Win32
BUILD: myos_v173_4_oop_nc_resize_frame_fix

Fixes the final one-pixel live-resize edge artifact after v173.3.

Root cause:
- v173.3 made the compositor fill the current OOP client area before retained child GDI.
- The remaining visible artifact was non-client/frame ownership: titlebar buttons and resize grip could paint over the outer edge during live resize.

Fix:
- draw_window() now starts with a current-size full-frame erase.
- The compositor still fills the APP_IPC_PROXY client area before child GDI.
- The outer frame outline is drawn as a final pass after titlebar buttons and resize grip.
- This keeps the right/bottom/top NC edge compositor-owned during live resize.

This is not DialogLab-specific; it is the generic classic-frame compositor path for all WindowManager windows.
