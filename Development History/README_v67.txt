myOS v67 - OOP ClipMenuLab + cross-process Clipboard/Menu/Accelerator runtime

BUILD: myos_v67_oop_clipmenu_clipboard_menu_accel

What changed
------------
- clip-menu-lab / clipmenu / clip-menu-lab.exe now launch through the gui-ipc subsystem.
- ClipMenuLab WinMain/WndProc/state live in the fork/exec child process.
- OpenClipboard/CloseClipboard/EmptyClipboard/SetClipboardData/GetClipboardData are child-side stubs backed by ProcessHost IPC to the parent/session clipboard.
- CreateMenu/CreatePopupMenu/AppendMenuA/SetMenu/TrackPopupMenu/CreateAcceleratorTableA/TranslateAcceleratorA are available in the child runtime and mirrored via IPC diagnostics.
- Ctrl+C, Ctrl+V, Ctrl+N accelerators translate inside the child GetMessage/DispatchMessage flow into WM_COMMAND.
- ClipMenuLab renders through the generic v63+ GDI IPC command buffer; the parent remains a generic frame/renderer.

Test
----
1. make clean && make
2. sudo chvt 3
3. sudo ./myos_input /dev/input/event1 /dev/input/event2
4. Open ClipMenuLab from Start Menu.

Expected:
- Window title: ClipMenuLab [OOP]
- Buttons: Set Clip / Get Clip / Clear / Attach Menu / Popup / Ctrl+C / Ctrl+V / Ctrl+N
- Set Clip writes a text like "Hello from OOP ClipMenuLab v67 #N" through the parent session clipboard.
- Get Clip reads it back through IPC.
- Ctrl+C/Ctrl+V/Ctrl+N produce WM_COMMAND through TranslateAcceleratorA in the child process.
- Close exits child with code 67.

Regression scope kept:
- OOP Calculator
- OOP Editor
- OOP PaintLab
- OOP ControlLab child-HWND controls
