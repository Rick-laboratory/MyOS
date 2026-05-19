myOS v225 - input route descriptor dispatch

Goal
----
Continue the USER32 state-machine rollout by making input routing explicit in
the private queue metadata.  Public MSG remains MSDN-shaped; the private
MyMessage sidecar now records why a target HWND won routing.

New private routing metadata
----------------------------
- _MSG_ROUTE_REASON_DIRECT
- _MSG_ROUTE_REASON_CAPTURE
- _MSG_ROUTE_REASON_FOCUS
- _MSG_ROUTE_REASON_HITTEST
- _MSG_ROUTE_REASON_HOVER
- _MSG_ROUTE_REASON_HOOK
- _MSG_ROUTE_REASON_ACCELERATOR
- _MSG_ROUTE_REASON_DIALOG
- _MSG_ROUTE_REASON_TIMER
- _MSG_ROUTE_REASON_THREAD
- _MSG_ROUTE_REASON_SYNTHETIC

New _MsgRouteDescriptor carries:
- lane
- input_kind
- route_state
- route_reason
- route_flags
- hwnd_action
- target_hwnd
- capture_hwnd
- focus_hwnd
- hit_hwnd

Behavior
--------
- Keyboard/char input routes through FOCUS.
- Mouse buttons/move route through HITTEST unless capture owns the route.
- Captured mouse messages carry CAPTURE route state/reason.
- Mouse wheel carries HOVER|HITTEST.
- Hooks can mark route metadata with HOOK.
- Queue selection still preserves public MSG shape; private routing metadata
  lives only in MyMessage/sidecar.

Validation
----------
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all
