myOS / Linux-Win32 v255 - Handle TLS free-batch
=================================================

v254 removed another ProcessLite PID/TID scan.  v255 moves back to the
handle-churn hot path and widens the v250 same-thread close->alloc reuse from a
single TLS slot to a small per-thread free batch.

Why
---
The previous v250 TLS free hint is perfect for strict close->alloc pairs:

    CloseHandle(h)
    DuplicateHandle(...)

But batchy GUI / loader / wait code often does this instead:

    close N temporary duplicates
    allocate N new temporary duplicates

With only one hint, the first new allocation can reuse the last close locally,
while the rest falls back to the process handle-table free stack.  v255 keeps a
small same-thread batch of freshly closed slots and consumes it before the
global free stack.

What changed
------------
* Added a per-thread `MyWinHandleFreeBatch` with 64 slots.
* `CloseHandle` now stores same-table freed slots in the local batch when
  possible, instead of immediately pushing them to the global free stack.
* Handle allocation now checks the TLS free-batch first, then the v250 one-slot
  hint, then the global free stack, then high-water allocation.
* If the current thread switches to another handle table, the previous batch is
  flushed back to that table's global free stack before the new table is used.
* A pthread TLS destructor flushes any remaining batch slots at thread exit.
* Existing generation/validity checks stay under the per-process handle-table
  lock.  The batch is only a local candidate source, not a semantic shortcut.

Diagnostics
-----------
`MyHandleTableAudit` now reports:

    handle_free_batch_hits
    handle_free_batch_stores
    handle_free_batch_flushes
    handle_free_batch_flushed_slots
    handle_free_batch_overflow
    handle_free_batch_misses

Smoke coverage
--------------
The strict handle smoke now has a v255 benchmark:

    v255 handle TLS free-batch reuses batchy close bursts
    v255 handle TLS free-batch benchmark

It closes a burst of duplicated handles and then reallocates the same number of
handles.  The expected path is batchStore >= N and batchHit >= N, proving the
burst reused local batch slots before the global free-stack path.

Compatibility notes
-------------------
The batch is conservative:

* It is same-thread and same-handle-table only.
* Slots are revalidated under the table lock before reuse.
* Overflow flushes to the normal global free stack.
* Thread exit flushes any remaining slots.
* Existing v249/v250 handle-cache and free-stack tests remain valid; v255 may
  satisfy the v250 close->alloc reuse through the wider batch instead of the
  original single-slot hint.
