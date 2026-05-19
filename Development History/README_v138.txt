BUILD: myos_v138_mdi_physical_menu_caption_route

Scope:
- Built on v137_mdi_command_origin_caption_drag_contract.
- Fixes the remaining gap between synthetic smoke checks and the real physical mouse path.

Main changes:
1. Raw physical left-button routing now tries the MDI child caption contract before falling back to generic top-level client routing.
   - Uses the exact screen point.
   - Finds the deepest child HWND.
   - Requires parent class MDICLIENT.
   - Sends WM_NCHITTEST to the MDI child.
   - If HTCAPTION is returned, sends WM_NCLBUTTONDOWN/HTCAPTION and consumes the raw down.

2. main.c no longer re-posts a second WM_LBUTTONDOWN when the raw router already consumed the click.
   - This closes the remaining physical double-dispatch gap.
   - App client clicks still work because normal client hits return handled=0 and are posted normally.

3. v137 command-origin behavior is retained.
   - Direct top-level app-menu command items still dispatch exactly one WM_COMMAND.
   - Toolbar BUTTON controls remain real child HWNDs.
   - No MSG._myos reintroduced.
   - v131 HWND access-control / MDI injection guards remain intact.

Validation:
make clean && make -j2
./myos_input --smoke all

Result:
SMOKE RESULT: PASS (0 failures)
