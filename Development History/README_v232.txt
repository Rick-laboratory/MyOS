myOS v232 - QS/Lane Slot-Bitset Queue Indexes
=============================================

v232 keeps the v231 hot/cold queue split and adds tiny per-thread queue slot
indexes so USER32 selectors no longer have to blindly walk the hot ring after a
QS prefilter hit.

Architecture changes:
- MyMessageQueue now owns 256-bit slot indexes over the 256-entry queue ring.
- Per-lane indexes exist for:
  - SEND
  - INPUT
  - WINDOW
  - POSTED
  - TIMER
  - BACKGROUND
- Per-QS-bit indexes exist for the public lower queue-status bits:
  - QS_KEY
  - QS_MOUSEMOVE
  - QS_MOUSEBUTTON
  - QS_POSTMESSAGE
  - QS_TIMER
  - QS_PAINT
  - QS_SENDMESSAGE
  - QS_HOTKEY
  - QS_ALLPOSTMESSAGE
  - QS_RAWINPUT
- find_match_slot_select() now intersects lane-slot and QS-slot bitsets, then
  jumps directly through candidate slots instead of testing every hot entry.
- Lane dispatch order is unchanged:
  SEND -> INPUT -> WINDOW -> POSTED -> TIMER -> BACKGROUND
- FIFO inside a lane is unchanged because candidate slots are consumed relative
  to queue->head, including wrapped rings.
- GetQueueStatus semantics remain unchanged: current_qs is LOWORD readiness,
  changed_qs is HIWORD since last matching query.

Index maintenance:
- Post adds the new physical tail slot to the lane/QS indexes.
- Coalescing clears and re-adds the affected slot.
- Remove/compaction rebuilds indexes from the live queue window so shifted slots
  cannot leave stale bits behind.

New smokes:
- [PASS] user32 v232 queue maintains QS/lane slot indexes
- [PASS] user32 v232 selector jumps through indexed candidate slots

Observed benchmark:
- [INFO] user32 v232 indexed queue slot-scan benchmark :: probes=2048 wall_ms=0.245 ops_s=8359184 candidates=2048 empty_skips=2048 slot_words=4 lane_index=192 qs_index=352

Validation:
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all

Result from this build:
- user32:         62 PASS, 0 FAIL, 0 WARN
- strict_handles: 85 PASS, 0 FAIL, 0 WARN
- all:            1250 PASS, 0 FAIL, 0 WARN
- SMOKE RESULT: PASS
