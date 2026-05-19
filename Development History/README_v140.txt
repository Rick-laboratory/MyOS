BUILD: myos_v140_mdi_nc_child_hit_sync_button_route

Purpose:
- Fix remaining manual MDILab problems after v139:
  1. MDI child caption drag still did not start reliably in the real evdev path.
  2. Real toolbar BUTTONs needed click-storming because fast physical down/up could arrive before the queued HWND pump established capture.

Changes:
- MDI caption hit-test now locates MDICLIENT and MDI children through real GetWindowRect-based window rectangles, not only ChildWindowFromPoint client recursion.
- Raw app-client mouse down/up is delivered synchronously to the app/control endpoint after non-client routing returns HTCLIENT.
- Captured app controls receive the matching synchronous up, so BUTTON capture/click state is established deterministically.
- Existing app menubar direct-command path from v137-v139 is left intact.
- v131 access-control/foreign MDI-injection protections are not relaxed.

New smoke coverage:
- MDILab toolbar fast click no-pump: raw down/up before hwnd_dispatch still creates exactly one child.
- Existing MDILab toolbar one-shot and physical caption drag gates remain green.

Validation:
make clean && make -j2
./myos_input --smoke all

Result:
SMOKE RESULT: PASS (0 failures)
