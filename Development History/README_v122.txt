myOS v122 - USER32 GetWindowRect / MoveWindow Contract

Build tag:
  myos_v122_user32_window_rect_move_contract

Goal:
  Remove the v120/v121 USER32 smoke WARNs for GetWindowRect and MoveWindow by
  making standalone USER32 HWND geometry queryable and movable even when no
  WindowManager/compositor slot exists yet.

What changed:
  - GetWindowRect now first uses the WindowManager/shared WindowState path for
    WindowManager-backed HWNDs.
  - USER32-created standalone HWNDs now fall back to USER32 metadata geometry.
  - Child HWND rects are converted from parent-client coordinates to screen
    coordinates for the public GetWindowRect contract.
  - SetWindowPos/MoveWindow now support local USER32 HWND metadata when no
    WindowManager slot exists.
  - The local path delivers WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_MOVE
    and WM_SIZE for the covered geometry update path.
  - GetWindowRect(NULL) and MoveWindow(NULL) are smoke-gated with GetLastError.

Smoke result:
  ./myos_input --smoke all
  SMOKE RESULT: PASS (0 failures)

Important:
  This is not a full USER32 conformance pass yet. It fixes the broken local
  HWND geometry contract and removes the old WARNs. The remaining bigger USER32
  work is still the message contract / public MSG strictness / DispatchMessage
  cleanup.
