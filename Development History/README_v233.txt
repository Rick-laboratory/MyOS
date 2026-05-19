myOS v233 - Selector Plan Op-Table + Input/Filter Slot Indexes

v233 continues the v231/v232 queue architecture cleanup.  v231 split the
message ring into hot/cold storage.  v232 added QS/lane slot-bitsets so the
selector can jump to candidate slots instead of blindly walking the hot ring.

v233 removes another layer of candidate-side branching:

- per-thread queue now also maintains 256-bit slot indexes for:
  - input-kind: none/key/char/mouse-move/mouse-button/mouse-wheel
  - filter-stage: hook/accelerator/dialog/translate/dispatch/menu/modeless
- selectors now compile once into a compact _QueueSelectPlan.
- lane, QS, input-kind and filter-stage constraints are consumed as index
  intersections.
- remaining non-indexed predicates, currently HWND and message range plus QS
  validation, run through a tiny predicate op-table.
- FIFO remains relative to queue->head, and lane order remains:
  SEND -> INPUT -> WINDOW -> POSTED -> TIMER -> BACKGROUND.
- GetQueueStatus LOWORD/HIWORD behavior is unchanged.

New smoke checks:

- [PASS] user32 v233 queue indexes input-kind and filter-stage slots
- [PASS] user32 v233 queue selector compiles a predicate op plan
- [PASS] user32 v233 input-kind selector is index-backed
- [PASS] user32 v233 filter-stage selector is index-backed

This is an architecture cleanup, not a test-driven behavior patch.  Tests were
updated to assert the intended path: selector constraints should become
bitset/index intersections whenever the queue can represent them as compact
state, and only unavoidable remaining filters should be predicate ops.
