myOS v61 - GUI IPC Runtime API Layer

BUILD: myos_v61_2_gui_ipc_no_autoclose

This version builds on v60's cross-process message queue.  The GUI IPC child
no longer drives the demo through raw shared-memory/socket structs directly.
Instead it has a tiny user32-like runtime stub layer inside the exec child:

  RegisterClassExA
  CreateWindowExA
  PostMessageA
  GetMessageA
  DispatchMessageA
  DestroyWindow

The stubs marshal scalar requests over the ProcessHost socketpair/shared
section bridge.  The parent/session process still owns the WindowManager and
HWND table, but the child now looks much closer to a normal Win32 GUI app.

Important:
  - normal GUI demo apps are still in-process for now
  - ipc-gui-lab is the out-of-process GUI prototype
  - v61 prepares moving calc/editor/etc. behind the same runtime stub surface

Also fixed:
  - processhost.c no longer ignores chdir() results in fork children
  - child chdir failure now prints a real error and exits with 125
  - build is clean with -Wall -Wextra -O2

Test:
  1. make clean && make
  2. sudo chvt 3
  3. sudo ./myos_input /dev/input/event1 /dev/input/event2
  4. open WaitLab
  5. click GUI IPC

Expected status contains roughly:
  GuiIPC: rt pid=... linux=... hwnd=... cw=1/1 q=... post=1/1 close=0 api=... gm=... dm=... last=...

The IPC proxy window should show:
  v61.1 GUI IPC Runtime proxy
  user32 stubs over IPC: CW/PM/GM/DM
  rt api=... cls=1 cw=1 gm=... dm=...


v61.2 hotfix:
- Removed fixed 240-iteration GUI IPC child demo loop.
- IPC GUI proxy stays alive until WM_CLOSE/DestroyWindow.
- Dragging no longer accelerates self-destruction via WM_WINDOWPOSCHANGED.
