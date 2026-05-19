myOS v83 - System Menu / WM_SYSCOMMAND Contract
================================================

BUILD: myos_v83_system_menu_syscommand_contract

Goal
----
Continue the MSDN-compliance pass after v77-v82:
window frame commands now share the Win32-style WM_SYSCOMMAND path.

What changed
------------
- Added non-client right-button messages:
  - WM_NCRBUTTONDOWN
  - WM_NCRBUTTONUP
- Added system command constants:
  - SC_KEYMENU
  - SC_RESTORE
- Added a per-window system menu model:
  - Restore
  - Move
  - Size
  - Minimize
  - Maximize
  - Close
- Right-click on a window titlebar/nonclient area opens the system menu.
- Alt+Space opens the system menu for the foreground window.
- System menu item clicks do NOT directly mutate the window.
  They route through WM_SYSCOMMAND:
  - Close    -> WM_SYSCOMMAND / SC_CLOSE
  - Minimize -> WM_SYSCOMMAND / SC_MINIMIZE
  - Restore  -> WM_SYSCOMMAND / SC_RESTORE
  - Maximize -> WM_SYSCOMMAND / SC_MAXIMIZE
  - Move     -> WM_SYSCOMMAND / SC_MOVE
  - Size     -> WM_SYSCOMMAND / SC_SIZE
- GetSystemMenu(HWND, BOOL) exists as a User32-lite API and returns a
  standard popup menu with SC_* command IDs.
- Version/build strings are updated to v83.

Test procedure
--------------
Run:

  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Basic regression:
1. Open Calc.
2. Click Calc client buttons: they must still work.
3. Drag the titlebar: window must still move.
4. Resize all edges/corners: v79 resize must still work.
5. X and minimize buttons must still work.

System menu:
1. Right-click the Calc titlebar.
   Expected: system menu appears at the cursor.
2. Click Minimize.
   Expected: Calc minimizes through WM_SYSCOMMAND / SC_MINIMIZE.
3. Restore Calc from taskbar or open system menu on another window.
4. Right-click titlebar -> Close.
   Expected: window closes through WM_SYSCOMMAND / SC_CLOSE.
5. Right-click titlebar -> Maximize.
   Expected: window fills work area above taskbar.
6. Alt+Space with a focused window.
   Expected: system menu appears near that window.

Stress/regression:
1. Open Spy++ and keep it visible.
2. Open AccessLab, PumpLab, DeadlockLab, DragLab, ServiceLab.
3. Move/resize each window.
4. Right-click each titlebar and try Minimize/Close.
5. Expected: no hang, no crash, Spy++/WSTS should keep updating.

Notes
-----
v83 intentionally does not add a full Windows System Menu object model
with per-window custom menu mutation yet. It establishes the contract:
all frame/menu commands route through WM_SYSCOMMAND, so later Alt+F4,
taskbar context menus, keyboard menu navigation, and custom system menus
can reuse the same path.
