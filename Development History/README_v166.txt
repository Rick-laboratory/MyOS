myOS v166 - OOP DragLab capture/drop ordering fix

BUILD: myos_v166_oop_drag_capture_order_fix
BASE:  myos_v165_process_isolation

Why this version exists
-----------------------
The v165 OOP DragLab correctly moved drag/capture state into the AppHost child
process, but the parent-side IPC proxy released mouse capture before forwarding
WM_LBUTTONUP to the child.  That inverted the Win32 capture/drop ordering:

    wrong order: WM_CAPTURECHANGED -> WM_LBUTTONUP
    desired:     WM_LBUTTONUP      -> WM_CAPTURECHANGED / release cleanup

Visible symptom
---------------
The dragged box could visibly sit inside DROP TARGET while the status line still
reported:

    MouseUp: WM_LBUTTONUP ... drop=no

That happened because WM_CAPTURECHANGED cleared the child drag state before the
button-up/drop verdict was computed.

Changes
-------
- window.c / ipcproxy_wndproc:
  - parent proxy still SetCapture()s on WM_LBUTTONDOWN so OOP child windows keep
    receiving drag traffic through the proxy HWND.
  - WM_LBUTTONUP is now forwarded to the child first.
  - parent proxy capture is released only after the child has received the
    button-up.

- myos_apphost_child.c / OOP DragLab:
  - capture-change cleanup no longer overwrites an already-published Drop/MouseUp
    verdict when the notification is the post-button-up release cleanup.
  - DragLab smoke now drags into the target and requires `drop=TARGET`.

- app_draglab.c:
  - in-process legacy DragLab capture-change handler also avoids hiding the last
    button-up verdict when capture was already clean.

- main.c:
  - top-left debug badge version string updated from stale v148 text to v166.

Validation
----------
make clean && make
./myos_input --smoke apphost
./myos_input --smoke all

Result observed in this package:
- Build PASS, no compiler warnings
- apphost smoke PASS: 13/13
- all smoke PASS: 1027 PASS, 0 FAIL, 0 WARN
