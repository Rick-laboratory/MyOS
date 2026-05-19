myOS v86.1 - START Button Hit/Command Fix
==========================================

BUILD: myos_v86_1_start_button_hit_fix

Purpose
-------
v86's dialog manager worked, but the START button had already been fragile for a
few versions.  The shell taskbar is now a real HWND (Shell_TrayWnd) and START is
a real BUTTON child HWND.  In some runtime states ChildWindowFromPoint() failed
to return the START child, so clicks fell through to the taskbar handler.  That
handler only handled task buttons and closed menus, so START appeared dead.

Fix
---
- Keep the MSDN-shaped route:

    raw mouse -> START BUTTON HWND -> ButtonWndProc -> WM_COMMAND(ID_TASKBAR_START)
    -> TaskbarWndProc -> wm_open_menu()

- Add a deterministic shell-hit fallback in wm_shell_hit_taskbar(): if the point
  is inside the START rectangle and the generic child hit-test misses, target
  wm->hwnd_start_button anyway.
- Add a safety net in TaskbarWndProc: if Shell_TrayWnd itself receives an
  WM_LBUTTONDOWN in the START rectangle, it posts WM_COMMAND(ID_TASKBAR_START)
  to itself instead of treating the click as an empty taskbar click.

Regression scope
----------------
No changes to the v86 dialog manager / tab order / IsDialogMessage path.
No changes to app-client click delivery.
No changes to nonclient move/resize.

Test procedure
--------------
1. sudo chvt 3
2. sudo ./myos_input /dev/input/event1 /dev/input/event2
3. Click START.
   Expected: Start menu opens/closes.
4. START -> Neuer Rechner.
   Expected: Calc opens.
5. Open ControlLab and test Tab / Shift+Tab / Space / Enter.
   Expected: v86 dialog-navigation still works.
6. Test Calc/Editor client clicks.
   Expected: still works.
7. Test Alt+Space menu and Alt+F4.
   Expected: still works.
8. Test taskbar app buttons.
   Expected: minimize/restore/focus still works.
