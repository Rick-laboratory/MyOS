myOS v78 - Nonclient Frame Hit-Test / WM_SYSCOMMAND
====================================================

Goal
----
Move app frame handling toward Win32/MSDN semantics.

v77 made the shell real HWNDs (#32769 Desktop, Shell_TrayWnd Taskbar,
START BUTTON).  v78 moves classic app-frame behavior out of direct compositor
hit-test actions and into a Win32-shaped chain:

  raw mouse
    -> top window
    -> WM_NCHITTEST
    -> HTCLIENT / HTCAPTION / HTCLOSE / HTMINBUTTON / HTRIGHT / HTBOTTOM ...
    -> WM_NCLBUTTONDOWN for non-client hits
    -> DefWindowProc-style default frame behavior
    -> WM_SYSCOMMAND for close/minimize/move/size

What changed
------------

1. Added Win32 non-client constants

   Messages:
     WM_NCHITTEST
     WM_NCMOUSEMOVE
     WM_NCLBUTTONDOWN
     WM_NCLBUTTONUP
     WM_SYSCOMMAND

   Hit-test codes:
     HTCLIENT
     HTCAPTION
     HTCLOSE
     HTMINBUTTON
     HTRIGHT
     HTBOTTOM
     HTBOTTOMRIGHT
     ... plus the standard HT* subset

   System commands:
     SC_CLOSE
     SC_MINIMIZE
     SC_MOVE
     SC_SIZE

2. Added wm_def_window_proc(...)

   DefWindowProcA now delegates non-client/system-command messages to the
   WindowManager-backed default frame handler for top-level myOS app HWNDs.

   This is currently a parent-side USER/frame bridge because the compositor
   still owns the classic titlebar/border.  It gives the same message shape
   without asking OOP child processes to know the parent frame geometry.

3. App-frame mouse down now runs through WM_NCHITTEST

   The old direct checks:

     hit_close(...)
     hit_minimize(...)
     hit_titlebar(...)

   are no longer executed as final actions directly in the raw mouse path.
   The router now computes:

     WM_NCHITTEST -> HT* result

   If the result is HTCLIENT, main.c still posts normal WM_LBUTTONDOWN with
   client-relative lParam to the app/control HWND.

   If the result is non-client, v78 calls the DefWindowProc-shaped frame path:

     WM_NCLBUTTONDOWN(wParam = HT*)

4. Close/minimize now become WM_SYSCOMMAND

   Close button:

     HTCLOSE -> WM_NCLBUTTONDOWN -> WM_SYSCOMMAND / SC_CLOSE

   Minimize button:

     HTMINBUTTON -> WM_NCLBUTTONDOWN -> WM_SYSCOMMAND / SC_MINIMIZE

5. Titlebar move now starts from HTCAPTION

   Titlebar drag path:

     HTCAPTION -> WM_NCLBUTTONDOWN -> default move loop

6. Right/bottom resize prepared through HT* codes

   Existing stable resize modes are kept:

     HTRIGHT       -> right resize
     HTBOTTOM      -> bottom resize
     HTBOTTOMRIGHT -> bottom-right resize

   Full left/top/corner resizing is intentionally left for v79 so this cut does
   not mix coordinate-origin-moving resize math into the non-client migration.

7. Client-click regression protection remains

   Calc/Editor/OOP app client clicks still go through the v77.3 fixed path:

     WM_LBUTTONDOWN/UP with lParam = client-relative coords

Test procedure
--------------

Build:

  make clean && make

Run:

  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Tests:

1. Version marker

   Top/debug text or stdout should show:

     myos_v78_nonclient_frame_hit_test

2. Calc client regression

   Open Calculator.
   Click number/operator buttons.

   Expected:
     display / last button / hit counter reacts like v77.3.

3. Editor client regression

   Open Editor.
   Click into the text/client area and type.

   Expected:
     caret/text input still works.

4. Close button path

   Click X on Calc/Editor/SurfaceLab.

   Expected:
     window closes.

   v78 path:
     WM_NCHITTEST -> HTCLOSE -> WM_NCLBUTTONDOWN -> WM_SYSCOMMAND(SC_CLOSE)

5. Minimize path

   Click minimize button.

   Expected:
     window minimizes and can be restored/focused from taskbar.

   v78 path:
     WM_NCHITTEST -> HTMINBUTTON -> WM_NCLBUTTONDOWN -> WM_SYSCOMMAND(SC_MINIMIZE)

6. Titlebar drag path

   Drag titlebar.

   Expected:
     window moves.

   v78 path:
     WM_NCHITTEST -> HTCAPTION -> WM_NCLBUTTONDOWN -> default move loop

7. Resize compatibility

   Drag right edge, bottom edge, bottom-right corner.

   Expected:
     existing resize behavior still works.

8. Shell regression

   START button, desktop right-click menu, taskbar item clicks must still work.

9. HWND StateProbe

   Open HWND StateProbe, Map/Subscribe.
   Move/minimize/close windows.

   Expected:
     lastMessage/serial should reflect non-client related updates such as
     WM_NCHITTEST, WM_NCLBUTTONDOWN, WM_SYSCOMMAND, WM_SHOWWINDOW.

Known limits
------------

- The compositor still owns frame drawing.
- OOP child apps are not yet asked to override WM_NCHITTEST themselves.
  The parent-side USER/frame bridge supplies default frame semantics.
- Full left/top/corner resizing is deferred to v79.
- System menu / Alt+Space / Alt+F4 are natural follow-ups after this.
