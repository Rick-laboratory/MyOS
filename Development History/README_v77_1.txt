BUILD: myos_v77_1_button_owner_command_fix

v77.1 fixes the v77 button-click regression.

Root cause:
  The built-in BUTTON control posted WM_COMMAND via public PostMessageA(),
  which uses the ambient thread-local MyWin runtime capability.  After Shell
  HWNDs (#32769/Shell_TrayWnd/START) became real HWNDs, that ambient capability
  could belong to shell/session/state setup or a different app.  Therefore
  BN_CLICKED could be posted with the wrong token or fail completely.

Fix:
  MyButtonWndProc now posts WM_COMMAND through an internal helper that builds
  a sender capability from the actual control HWND owner pid.  This is closer
  to Win32 semantics: a control notifies its parent as its owning window/thread,
  not as whichever subsystem last called MyWinBindRuntime().

Terminology cleanup:
  wm_input_mouse_button_down/up were renamed to
  wm_route_raw_mouse_button_down/up.  They are not Win32 APIs.  They are only
  the raw Linux-input-to-HWND routing shim below the real WM_LBUTTONDOWN /
  WM_LBUTTONUP / WndProc pipeline.

Test:
  1. Start OS.
  2. Click START button. Menu should open/close.
  3. Open HWND StateProbe. Its Map/Refresh/CloseMap/Subscribe buttons should visibly update counters.
  4. Open WaitLab/SectionLab/SurfaceLab and click their buttons.
  5. Start menu entries should still launch apps through WM_COMMAND.
