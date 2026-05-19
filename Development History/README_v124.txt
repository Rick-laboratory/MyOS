BUILD: myos_v124_user32_lifecycle_message_order_contract

Goal:
  USER32 lifecycle/message-order contract pass on top of v123 public MSG layout.

Focus:
  - CreateWindowExA lifecycle ordering is now smoke-gated:
      WM_NCCREATE -> WM_CREATE -> WM_SHOWWINDOW for WS_VISIBLE windows.
  - WM_NCCREATE FALSE abort path is tested.
  - WM_CREATE returning -1 now aborts CreateWindowExA and tears down the partially-created HWND.
  - DestroyWindow ordering is smoke-gated:
      WM_DESTROY -> WM_NCDESTROY.
  - WM_CLOSE is tested as an application decision point:
      custom WndProc can ignore it;
      DefWindowProcA(WM_CLOSE) destroys the HWND.
  - PostMessage/PeekMessage/DispatchMessage FIFO behavior is smoke-gated for normal-priority messages.
  - PeekMessageA(PM_NOREMOVE) is tested to not remove.
  - GetMessageA returns FALSE on WM_QUIT and preserves PostQuitMessage exit code.
  - PostQuitMessage is now declared in the public SDK winuser.h.

Important compliance fix:
  CreateWindowExA no longer treats lpParam as an internal myOS thunk pointer.
  lpParam is now app-owned data and is passed through CREATESTRUCTA::lpCreateParams,
  which is the Win32/MSDN contract.

Validation:
  make clean && make -j2
  ./myos_input --smoke all

Expected:
  BUILD: myos_v124_user32_lifecycle_message_order_contract
  SMOKE RESULT: PASS (0 failures)

New smoke group:
  lifecycle 32 checks, 32 pass, 0 fail, 0 warn

Existing groups remain green:
  kernel32       29 pass
  user32         24 pass
  gdi            18 pass
  menu           24 pass
  capture        12 pass
  ipc_section    13 pass
  access         50 pass
  handle_invalid 14 pass
  wait_invalid   12 pass
  last_error      9 pass
  comdlg          4 pass
  services        6 pass
  apphost         5 pass
