# myOS v224 - Input-kind message queue / HWND action dispatch

v224 continues the USER32 state-machine rollout where it should have started:
raw input and queued messages are now classified beyond the broad INPUT lane.

## Changes

- Public MSG remains MSDN-shaped.
- Queue-private MyMessage now stores:
  - input_kind
  - route_state
  - route_flags
- Added private input kinds:
  - _MSG_INPUT_KEY
  - _MSG_INPUT_CHAR
  - _MSG_INPUT_MOUSE_MOVE
  - _MSG_INPUT_MOUSE_BUTTON
  - _MSG_INPUT_MOUSE_WHEEL
- Added route states:
  - _MSG_ROUTE_TARGET_RESOLVED
  - _MSG_ROUTE_CAPTURED
  - _MSG_ROUTE_FOCUS
  - _MSG_ROUTE_HOOKED
  - _MSG_ROUTE_BLOCKED
- Queue selectors can now filter by input kind via _QUEUE_SELECT_INPUT_KIND.
- Dispatch maps queued input metadata to HWND action requirements:
  - mouse input requires MESSAGE + HITTEST
  - keyboard/char input requires MESSAGE + FOCUS
- Manual DispatchMessageA synthesizes the same private sidecar metadata from public MSG fields.

## Why

Message queues are state machines.  v223 introduced lanes.  v224 splits the
INPUT lane into the concrete dispatch categories that later hook/capture/focus
routing can consume directly instead of rediscovering intent through WM_* if
ladders.

## Validation

- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all
