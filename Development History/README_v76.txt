myOS v76 - StartMenu WM_COMMAND/Menu Refactor
=============================================

Goal
----
Version 76 fixes an architectural Win32 mismatch in the desktop shell:
wm_mouse_down() no longer contains the Start-menu item action blob.  The Start
menu now selects a named command ID, closes the menu, and routes the command
through a desktop WM_COMMAND-style dispatcher.

What changed
------------
1) Start-menu command IDs
   - ID_START_NEW_TERMINAL, ID_START_CALC, ID_START_SURFACELAB, ...
   - no more magic iy == N checks for menu actions.

2) Start-menu item table
   - each visible item has { id, text, flags }.
   - separators have START_MENU_SEPARATOR and never trigger commands.

3) Mouse handler simplified
   - wm_mouse_down() only does menu open/close, menu hit-test, taskbar/window/icon mouse logic.
   - menu action code moved into wm_desktop_command().

4) Desktop WM_COMMAND path
   - selected menu item -> wm_post_desktop_command(...)
   - wm_desktop_wndproc(... WM_COMMAND ...)
   - wm_desktop_command(...) runs the real action.

5) Future-proofing
   - accelerators/keyboard shortcuts can later post the same command IDs.
   - the Start menu and future popup menus can share the command dispatch path.

Build
-----
    make clean && make

Expected build marker:
    BUILD: myos_v76_startmenu_wm_command_menu_refactor

Run
---
    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2

Test procedure
--------------
A) Open menu from Start button
   1. Click START.
   2. Menu opens above taskbar.
   3. Click "Neuer Rechner".
   Expected: Calc starts.

B) Open menu from desktop right-click
   1. Right-click empty desktop.
   2. Menu opens at cursor.
   3. Click "SurfaceLab".
   Expected: SurfaceLab starts near cursor.

C) Separator safety
   1. Open menu.
   2. Click a separator line.
   Expected: Menu closes, no app starts.

D) Outside-click safety
   1. Open menu.
   2. Click outside the menu.
   Expected: Menu closes, no command runs.

E) Command action coverage
   Test these items at minimum:
   - Neues Terminal
   - Neuer Rechner
   - Neuer Texteditor
   - HWND StateProbe
   - SurfaceLab
   - Desktop neu laden
   - Iconmodus: Free/Grid
   - Hintergrund: Dunkel/Blau/Lila/Teal

F) Regression check
   - Taskbar buttons still minimize/restore/focus windows.
   - Desktop icon double-click still opens editable files.
   - Existing OOP apps still launch.

Important note
--------------
This version intentionally does not push the DWM surface work further.  v76 is
an architecture cleanup pass: Start menu commands now use a Win32-like
WM_COMMAND route instead of app-launch logic being buried inside the mouse
handler.
