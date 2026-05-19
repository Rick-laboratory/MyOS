myOS v86 - Dialog Manager / Tab Order / IsDialogMessage
========================================================

BUILD: myos_v86_dialog_manager_tab_order_isdialogmessage

Goal
----
Continue the MSDN-compliance migration after v84/v85:
keyboard input now has a first dialog-manager layer for child controls.

New/changed pieces
------------------
- WM_GETDLGCODE / DLGC_* constants added.
- SetFocus() / GetFocus() added.
- GetNextDlgTabItem() added.
- IsDialogMessageA() added.
- Built-in BUTTON handles:
  - WM_GETDLGCODE -> DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON
  - WM_SETFOCUS / WM_KILLFOCUS
  - Space -> pressed/release -> BN_CLICKED
  - Enter -> BN_CLICKED
- Built-in BUTTON focus outline is drawn by MyDrawChildWindows().
- Raw keyboard path calls IsDialogMessageA() for focused top-level HWNDs before debug F-key fallback.
- Esc closes the focused dialog/window through WM_CLOSE when a focused window exists; otherwise it still exits the PoC shell.

Test procedure
--------------
Start:

    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2

Core regression:
1. Top badge should say "v86 dialog keys".
2. Calc mouse buttons still work.
3. Editor click + typing still works.
4. Alt+Space opens system menu; Up/Down/Enter/Esc still works from v85.
5. Alt+F4 closes the active window, plain F4 still opens the debug terminal.
6. Move/resize/minimize/close still work.

Dialog/control keyboard test:
1. Open ControlLab [OOP child-HWND] from the menu.
2. Click once inside the window to focus it.
3. Press Tab.
   Expected: focus outline moves to Ping/Toggle BUTTON child HWND.
4. Press Tab again / Shift+Tab.
   Expected: focus moves forward/backward through BUTTON child HWNDs.
5. Press Space on focused button.
   Expected: visual pressed state and WM_COMMAND / BN_CLICKED count changes.
6. Press Enter on focused button.
   Expected: WM_COMMAND / BN_CLICKED count changes.
7. Press Esc.
   Expected: active focused window closes via WM_CLOSE.

Notes
-----
This is a first dialog-manager layer, not a full dialog box manager yet.
Classic hand-rolled labs that do not create real USER32 child HWNDs may still keep their own keyboard logic. The important new infrastructure is now present for BUTTON child HWNDs and future EDIT/STATIC/LISTBOX compliance work.
