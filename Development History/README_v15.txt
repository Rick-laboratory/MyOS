myOS v15 - WinAPI subscriptions / window APIs

Build tested:
  make clean && make

Main changes:
  - Added WinAPI-style window helpers:
      IsWindow
      GetForegroundWindow
      GetWindowRect
      SetWindowPos
      GetWindowTextA / SetWindowTextA
      FindWindowA
  - Added RECT / LPRECT and SetWindowPos flags in mywin.h.
  - Added MyWindowState snapshot struct.
  - Added MyWinBindDesktop(WindowManager*) so the user32-like layer can reach desktop/window state.
  - Added myOS window-message subscriptions:
      MySubscribeWindowMessage
      MyUnsubscribeWindowMessage
      MyGetWindowState
  - HWNDManager now tracks subscription count.
  - Debug HUD now shows SUB count.
  - Moving/resizing/minimizing publishes WM_WINDOWPOSCHANGED through the HWND message router.

Design note:
  The queue still carries only events; current state is read via MyGetWindowState/GetWindowRect.
