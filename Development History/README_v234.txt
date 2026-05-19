myOS v234 - Cached selector plan + index-source mask

Goal
----
Continue the USER32 queue architecture after v233 without bending tests back to
old branchy paths.  v233 moved lane/QS/input-kind/filter-stage selection into
slot indexes plus a compact predicate plan.  v234 makes that plan a reusable
queue-local compiled object.

What changed
------------
- _QueueSelectPlan now carries indexSourceMask:
  - _QUEUE_SELECT_SOURCE_QS
  - _QUEUE_SELECT_SOURCE_INPUT_KIND
  - _QUEUE_SELECT_SOURCE_FILTER_STAGE
- Input-kind and filter-stage stay explicit slot-index sources, not predicate
  ops.
- MyMessageQueue now caches the last compiled _QueueSelectPlan plus the select
  signature used to build it.
- Repeated same-shape PeekMessage/GetMessage selectors reuse the compiled plan.
- Changing selector shape (HWND/range/lane/input-kind/filter-stage/QS/remove)
  invalidates the cache and recompiles.

Why this matters
----------------
The queue selector is now split into narrow state-machine stages:

  _QueueSelect
    -> cached _QueueSelectPlan
    -> indexSourceMask selects slot-index inputs
    -> lane/QS/input/filter bitset intersection
    -> compact predicate op table for remaining filters

This removes repeated plan compilation from the hot pump path without weakening
FIFO, lane-order, QS, hwnd, or message-range semantics.

New smoke coverage
------------------
- v234 queue selector caches compiled plans
- v234 selector plan cache invalidates by select signature
- v234 cached selector-plan benchmark

Validation
----------
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all
