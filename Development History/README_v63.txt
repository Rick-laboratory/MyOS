myOS v63 - Cross-process WM_PAINT + GDI command buffer

BUILD: myos_v63_1_gdi_ipc_textcopy_fix

Goal
----
v62 proved that the Calculator WinMain/WndProc/state can live in a real
fork/exec GUI child. v63 removes the biggest remaining Calculator special case:
the parent no longer needs to know how to paint Calculator state directly.

New architecture
----------------
calc / calculator:
  ShellExecute/AppHost
    -> Process-Lite
    -> fork/exec myos_apphost_child --gui calc
    -> child RegisterClassExA/CreateWindowExA over IPC
    -> parent creates HWND/Desktop frame
    -> parent routes WM_PAINT/WM_LBUTTONDOWN/WM_CLOSE over IPC
    -> child GetMessage/DispatchMessage calls the Calculator WndProc
    -> child writes GDI commands into the shared section
    -> parent generically renders the GDI command buffer

Shared GDI command buffer
-------------------------
process_ipc.h now defines:
  MyGdiIpcCommand
  MYOS_GDI_OP_FILLRECT
  MYOS_GDI_OP_RECTANGLE
  MYOS_GDI_OP_TEXTOUT
  MYOS_GDI_OP_DRAWTEXT

The shared process section now carries:
  gdi_enabled
  gdi_sequence
  gdi_paint_count
  gdi_command_count
  gdi_client_w / gdi_client_h
  gdi_status
  gdi_commands[MYOS_GDI_MAX_COMMANDS]

Calculator behavior
-------------------
The Calculator child handles WM_CREATE/WM_PAINT/WM_WINDOWPOSCHANGED/
WM_LBUTTONDOWN/WM_CLOSE. On paint or state changes it emits GDI commands:
  background FillRect
  display FillRect + Rectangle + DrawText
  history panel commands
  button rectangles/text
  small diagnostic footer

The parent APP_IPC_PROXY renderer now prefers the generic GDI command buffer.
The old v62 compact calc state remains only as a fallback/diagnostic path.

Test
----
make clean && make
sudo chvt 3
sudo ./myos_input /dev/input/event1 /dev/input/event2

Open Calculator from startup/F3/start menu. It should be an OOP GUI child,
painted through the generic GDI IPC buffer. Clicking buttons should update the
child state and repaint through a new GDI sequence.

Smoke tested
------------
A headless child smoke test exercised:
  child CreateWindowExA over IPC
  parent ACK
  child initial GDI buffer
  parent WM_PAINT message
  parent WM_LBUTTONDOWN on key 7
  child display becomes 7
  GDI sequence increments
  WM_CLOSE exits child with code 63
