myOS v66 - Cross-process child HWND controls
============================================
BUILD: myos_v66_cross_process_child_hwnds_controls

What changed
------------
- control-lab / controllab / control-lab.exe now run through the gui-ipc fork/exec path.
- A GUI child can call CreateWindowExA(... WS_CHILD ..., hWndParent, hMenu=id, ...) from inside the Linux child process.
- ProcessHost carries a new CREATE_CHILD_WINDOW / CHILD_WINDOW_ACK IPC path.
- Parent/session creates real USER32 child HWNDs below the IPC proxy top-level HWND.
- Parent hit-testing can now target child HWNDs via ChildWindowFromPoint.
- BUTTON child controls post WM_COMMAND/BN_CLICKED back to the parent HWND, which is routed over IPC to the child WndProc.
- Parent renderer draws generic GDI command buffer and overlays parent-side child controls with MyDrawChildWindows.

Test
----
1. make clean && make
2. sudo chvt 3
3. sudo ./myos_input /dev/input/event1 /dev/input/event2
4. Open ControlLab from the start/menu.
5. You should see an OOP ControlLab window with real parent-created child BUTTON HWNDs.
6. Clicking Ping should update the child-side status via WM_COMMAND.

Regression targets
------------------
- calc remains OOP GUI child.
- editor remains OOP GUI child.
- paint-lab remains OOP GUI child with GDI line buffer.
- ipc-gui-lab remains the low-level proxy test.
