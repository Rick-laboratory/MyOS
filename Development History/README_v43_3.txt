myOS v43.3 - USER32 close hardening

Fixes the v43/v43.1/v43.2 regression where USER32-backed apps such as
Calculator, TextEditor, ClipMenuLab, PaintLab, DragLab and ControlLab could
survive the shell X close path.

Root cause:
- Terminal and several legacy labs used the old direct HWND close path.
- USER32-backed top-level apps route through CreateWindowExA/class WndProc.
- Some older lab WndProcs returned 0 for unhandled messages instead of falling
  through to DefWindowProcA.
- v43.2 assumed surviving WM_CLOSE meant "app vetoed close", leaving the shell
  frame alive.

v43.3 behavior:
- Shell close still first sends WM_CLOSE synchronously.
- If the HWND still exists afterward, the shell calls USER32 DestroyWindow(hwnd).
- This preserves child destruction, WindowInfo cleanup, WM_DESTROY/WM_NCDESTROY,
  and WindowManager slot sync.
- ClipMenuLab and PaintLab now default to DefWindowProcA for unhandled messages.

Build tested:
  make clean && make

Smoke test:
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Close-test list:
  Calculator, TextEditor, ClipMenuLab, PaintLab, DragLab, ControlLab,
  Terminal, ServiceLab, WaitLab, SectionLab, ObjectLab.
