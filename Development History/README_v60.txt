myOS v60 - Cross-process Message Queue

Goal:
  Build on v59 GUI CreateWindowExA-over-IPC and add the first real
  parent<->child message bridge: PostMessage/GetMessage/DispatchMessage style.

What changed:
  - ProcessHost can send scalar WINDOW_MESSAGE packets to a real GUI child.
  - The GUI child keeps a local message queue and runs GetMessage/DispatchMessage-lite.
  - The child can issue a PostMessage request back to the parent; the parent queues it
    into the HWND/UI-thread queue and ACKs it.
  - The parent-side IPC proxy WndProc forwards WM_CREATE/WM_PAINT/WM_CLOSE/etc. to
    the child process by owner PID.
  - Shared section diagnostics now expose sent/received/dispatched/post/ack/close state.

Test:
  1. make clean && make
  2. sudo chvt 3
  3. sudo ./myos_input /dev/input/event1 /dev/input/event2
  4. Open WaitLab
  5. Click GUI IPC

Expected:
  - An IPC GUI proxy window appears.
  - The window shows message counters: sent/recv/disp/post/ack.
  - Closing the window sends WM_CLOSE to the child; the child reports close_seen and exits.

Still not done intentionally:
  - Full cross-process WndProc callbacks for arbitrary GUI apps.
  - Pointer payload marshalling.
  - Shared render surfaces.
  - Blocking SendMessage across process with reply object.
