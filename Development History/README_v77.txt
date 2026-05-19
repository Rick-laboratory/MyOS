myOS v77 - Shell HWNDs / WndProc Input Pipeline
================================================

Goal
----
Move the shell UI toward Win32/MSDN structure:

  raw input -> shell/app HWND -> WndProc -> WM_COMMAND / message handling

Instead of treating the desktop/taskbar/start menu as private compositor state.

What changed
------------

1. Real shell HWNDs

   v77 creates real USER32-lite windows for the shell:

     #32769          DesktopWndProc
     Shell_TrayWnd   TaskbarWndProc
     BUTTON          START child HWND, parented to Shell_TrayWnd

   The HWND StateProbe should now show entries similar to:

     Desktop
     Taskbar
     START

2. GetDesktopWindow()

   Added GetDesktopWindow() to the public WinAPI-lite surface.
   It returns the real #32769 desktop HWND.

3. Input routing no longer has wm_mouse_down()

   The old public wm_mouse_down() function is gone.
   main.c now calls:

     wm_route_raw_mouse_button_down(...)
     wm_route_raw_mouse_button_up(...)

   Those functions are routers, not desktop/taskbar behavior handlers.

4. Desktop behavior moved into DesktopWndProc

   DesktopWndProc handles:

     WM_RBUTTONDOWN  -> open desktop/start menu
     WM_LBUTTONDOWN  -> desktop icon select/drag/open, or menu item click
     WM_COMMAND      -> dispatch Start menu command IDs

5. Taskbar behavior moved into TaskbarWndProc

   TaskbarWndProc handles:

     WM_COMMAND / ID_TASKBAR_START -> open/close start menu
     WM_LBUTTONDOWN               -> taskbar item focus/minimize toggle

   The START button itself is a real BUTTON child HWND.  Click path:

     raw mouse
       -> ChildWindowFromPoint(Shell_TrayWnd)
       -> BUTTON WndProc
       -> WM_COMMAND(ID_TASKBAR_START)
       -> TaskbarWndProc
       -> wm_open_menu(...)

6. v78 boundary is explicit

   Classic app frame/titlebar handling still exists as a compatibility path:

     close/minimize/titlebar-drag/resize

   v78 should move that into real non-client semantics:

     WM_NCHITTEST
     WM_NCLBUTTONDOWN
     WM_SYSCOMMAND
     DefWindowProc frame behavior

Test procedure
--------------

Build:

  make clean && make

Run:

  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Tests:

1. Version marker

   Top/debug output should say:

     v77 Shell HWND/WndProc input
     BUILD: myos_v77_1_button_owner_command_fix

2. Start button path

   Click START.

   Expected:

     menu opens

   Important: this should now go through:

     BUTTON child HWND -> WM_COMMAND(ID_TASKBAR_START) -> TaskbarWndProc

3. Desktop right click

   Right-click empty desktop.

   Expected:

     menu opens at cursor

   This is now DesktopWndProc::WM_RBUTTONDOWN.

4. Menu command path

   Open menu and click:

     Neuer Rechner
     SurfaceLab
     HWND StateProbe

   Expected:

     apps launch as before

   The command route is now:

     menu hit -> WM_COMMAND -> DesktopWndProc -> wm_desktop_command

5. Taskbar item path

   Click taskbar buttons for open windows.

   Expected:

     focused window changes or focused window minimizes/restores

   This is now TaskbarWndProc::WM_LBUTTONDOWN.

6. HWND StateProbe

   Open HWND StateProbe and Map/Subscribe.

   Expected visible shell HWND entries:

     class/title equivalent of Desktop (#32769)
     Taskbar (Shell_TrayWnd)
     START (BUTTON child)

   The visible titles should at least include:

     Desktop
     Taskbar
     START

7. App client mouse path regression

   Open Calc or Editor and click inside the client area.

   Expected:

     app still receives WM_LBUTTONDOWN/UP with MSDN lParam mouse coords.

8. Frame compatibility path

   Move/resize/close/minimize windows.

   Expected:

     works as before

   Note: this is intentionally still a v78 target for true WM_NCHITTEST /
   WM_NCLBUTTONDOWN compliance.

Known limits
------------

- Start menu itself is still rendered by the shell/compositor, not a real popup
  HWND/TrackPopupMenu surface yet.
- App frame non-client hit-testing is still the compatibility path and should
  become v78.
- This version focuses on making Shell Desktop/Taskbar/Start participate in
  the HWND/WndProc/message pipeline.
