myOS v171 - OOP DialogLab WM_COMMAND activation filter

BUILD: myos_v171_dialog_command_filter

Fix:
- OOP DialogLab no longer treats every WM_COMMAND notification as a dialog action.
- Win32 BUTTON controls send WM_COMMAND on focus changes too: BN_SETFOCUS / BN_KILLFOCUS.
- v170 could therefore open two dialogs from one physical click:
  mouse-down -> BN_SETFOCUS -> command executed
  mouse-up   -> BN_CLICKED  -> command executed again
- v171 filters OOP DialogLab commands so only activation notifications execute:
  HIWORD(wParam) == BN_CLICKED / code 0.

Why this is the compliant direction:
- BN_SETFOCUS and BN_KILLFOCUS are notifications, not button activations.
- IDOK / IDCANCEL / command buttons must react to BN_CLICKED, menu/accelerator code0 paths, Enter/Esc dialog semantics — not mere focus movement.
- This keeps the real BUTTON class behavior intact instead of neutering controls or shaping architecture around the smoke tests.

Smoke change:
- AppHost smoke now uses a physical BUTTON mouse-down/up path for the OOP DialogLab modeless button.
- Canary: "OOP DialogLab physical BUTTON opens one modeless HWND".
- This catches the exact double-command class of bug without limiting the architecture.

Verified locally:
- make with CFLAGS='-Wall -Wextra -O0 -D_GNU_SOURCE -Isdk/include'
- ./myos_input --smoke apphost
- ./myos_input --smoke all

Expected:
- SMOKE RESULT: PASS (0 failures)
