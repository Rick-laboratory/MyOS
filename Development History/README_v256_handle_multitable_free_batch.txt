v256 - Handle TLS multi-table free-batch
========================================

Focus
-----
Continue the v255 handle-churn work by removing the remaining table-switch
flush in the per-thread free-slot cache.

What changed
------------
- The v255 one-table TLS handle free-batch is now a four-lane per-thread cache.
- Each lane keeps recently closed slots for one MyWinHandlePidTable.
- Alloc/close paths can alternate between current, child, and foreign process
  handle tables without flushing the old table's local batch every time.
- Slots are still consumed while the owning process handle-table lock is held;
  this is a locality/lock-pressure optimization, not a semantic shortcut.
- Destructor-time cleanup flushes remaining lanes back to the marked global
  free stack.
- Process sweep/table teardown drops any current-thread lane for that table.

New diagnostics
---------------
MyHandleTableAudit now includes:
- handle_free_batch_lane_allocs
- handle_free_batch_lane_matches
- handle_free_batch_table_switch_avoided

New smoke coverage
------------------
strict_handles now includes:
- v256 handle TLS free-batch keeps multiple process tables warm
- v256 multi-table handle TLS free-batch benchmark

Expected impact
---------------
This should mainly help bursty DuplicateHandle/CloseHandle patterns that move
handles between a parent/current process and one or more child process tables.
Single-table same-thread churn should remain covered by the v255 behavior.
