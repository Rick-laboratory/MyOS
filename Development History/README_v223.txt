# myOS v223 - Message queue lane/state dispatch

v223 continues the USER32 state-machine rollout by moving the thread message
queue itself toward the same model: public `MSG` remains MSDN-shaped, while the
private queue stores dispatch metadata that is computed once at enqueue time.

## What changed

- Adds private `_MSG_LANE` enum:
  - `_MSG_LANE_SEND`
  - `_MSG_LANE_INPUT`
  - `_MSG_LANE_WINDOW`
  - `_MSG_LANE_POSTED`
  - `_MSG_LANE_TIMER`
  - `_MSG_LANE_BACKGROUND`
- Adds private `_MSG_STATE` enum:
  - `_MSG_STATE_FREE`
  - `_MSG_STATE_QUEUED`
  - `_MSG_STATE_DISPATCHING`
  - `_MSG_STATE_DISPATCHED`
  - `_MSG_STATE_COALESCED`
- `MyMessage` now carries `lane` and `state` metadata.
- `myqueue_post()` classifies public `WM_*` messages into private lanes once.
- Queue iteration now walks a fixed lane dispatch order instead of only priority
  conditionals.
- Adds `_QUEUE_SELECT`, a private selector that describes which fields the queue
  iterator should match:
  - HWND filter
  - message range
  - lane mask
  - remove/wait behavior
- Keeps public `MSG`, `PeekMessageA`, `GetMessageA`, and `DispatchMessageA`
  behavior stable.

## Why

The message queue is a state machine.  v223 begins moving that truth into data:
`MSG` numbers become queue lanes, and queue selection becomes an explicit
selector record instead of scattered ad-hoc checks.

## Validation

- `make clean && make -j$(nproc)`
- `./myos_input --smoke user32`
- `./myos_input --smoke strict_handles`
- `./myos_input --smoke all`
