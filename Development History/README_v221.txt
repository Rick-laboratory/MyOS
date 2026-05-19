# myOS v221 - HWND state-machine action dispatch

v221 starts the broad state-machine rollout on the USER32 side.  v219 made
HWNDs slot/generation/state encoded; v221 adds a central private action table
so resolved HWND state selects whether query/message/mutate/show/focus/capture/
paint/destroy operations are legal.

This is not a public SDK surface.  Public WinAPI names remain public/MSDN-shaped;
internal USER32 edges use the private underscore nomenclature from v220:

- `_HWND_STATE_*` for window lifecycle state
- `_HWND_ACTION_*` for internal action classes
- `hwnd_state_allows()` for state -> action permission
- `hwnd_query_action()` for HWND -> header + state/action validation

USER32 paths moved onto the action context in this pass:

- `mywin_can_read_window()` -> `_HWND_ACTION_QUERY`
- `mywin_can_control_window()` -> `_HWND_ACTION_MUTATE`
- `mywin_can_message_window()` -> `_HWND_ACTION_MESSAGE`
- `DestroyWindow()` -> `_HWND_ACTION_DESTROY`
- `ShowWindow()` -> `_HWND_ACTION_SHOW`
- `SetFocus()` -> `_HWND_ACTION_FOCUS`
- `IsWindowVisible()` -> `_HWND_ACTION_QUERY`
- manual public `DispatchMessageA()` -> `_HWND_ACTION_MESSAGE`

This is the first slice, not the whole USER32 rewrite.  The point is to make
the resolved state the dispatch path so later v222+ work can move paint,
input/capture, menu/modal, dialog, MDI and GDI invalidation out of scattered
ad-hoc checks and into action/state tables.

Validation:

- `make clean && make -j$(nproc)`
- `./myos_input --smoke user32`
- `./myos_input --smoke strict_handles`
- `./myos_input --smoke all`

Observed smokes in this build:

- `user32`: 29 PASS, 0 FAIL, 0 WARN
- `strict_handles`: 85 PASS, 0 FAIL, 0 WARN
- `all`: PASS, 0 FAIL
