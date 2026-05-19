BUILD: myos_v123_user32_msg_public_contract

Goal:
  USER32 message contract cleanup on top of v122 geometry and v121 access-rights work.

What changed:
  - Public sdk/include/winuser.h MSG is MSDN-shaped again:
      HWND hwnd;
      UINT message;
      WPARAM wParam;
      LPARAM lParam;
      DWORD time;
      POINT pt;
  - Removed the public winuser.h dependency on ../../myqueue.h.
  - Removed the embedded MyMessage MSG._myos payload from the public SDK layout.
  - Added an internal USER32 thread-local dispatch sidecar in winuser.c.
  - GetMessageA/PeekMessageA still preserve private queue metadata internally for DispatchMessageA.
  - Manually constructed public MSG values now dispatch through DispatchMessageA using only public fields.

Smoke additions:
  - user32 now checks that sizeof(MSG) is small/public-contract sized.
  - user32 now checks DispatchMessageA with a manually constructed MSG.

Verification:
  make clean && make -j2
  ./myos_input --smoke all

Expected:
  BUILD: myos_v123_user32_msg_public_contract
  SMOKE RESULT: PASS (0 failures)

Known remaining larger USER32 debt:
  - Cross-thread SendMessageTimeout still uses the old HWND transport in places.
  - GetMessage/PeekMessage filtering is still myOS queue-backed and not a full Win32 thread-message model.
  - Focus/capture are still global instead of desktop/thread scoped.
  - Dialog/focus/capture core should be the next dedicated pass.
