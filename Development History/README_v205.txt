myOS v205 - sparse 24-bit per-process handle tables
====================================================

Goal
----
Remove the old 256-entry global public HANDLE table as an architectural limit.
Public KERNEL32 handles remain process-local table handles, but each process now
has a sparse 24-bit slot space (16,777,215 addressable slots) backed by
page-table-like allocation instead of a giant static array.

What changed
------------
- Replaced the single static g_HandleTable[256] with per-PID handle tables.
- Added sparse 3-level slot lookup using 8/8/8-bit indices:
  top index -> mid page -> leaf page -> handle entry.
- Public HANDLE encoding is now:
  bit 31     : myOS user-handle tag
  bits 30-24 : 7-bit slot generation counter
  bits 23-0  : 24-bit process-local slot index
- The PID is no longer required inside the HANDLE value for normal lookup.
  The owning process table selects the namespace, like real per-process HANDLE
  tables do. Cross-process operations already know the source/target process
  table from process handles.
- Close/reuse preserves and increments slot generation, so immediately stale
  handles no longer resolve after a close/reuse cycle.
- Handle allocation now uses per-process allocation hints, not a global linear
  256-slot scan.
- Handle enumeration/audit/inheritance/sweeps were rewritten to walk/snapshot
  the sparse tables instead of iterating a fixed array.

Smoke updates
-------------
The strict handle smoke now duplicates one event handle 300 times in the same
process. This intentionally crosses the old 256-slot wall and proves that the
architecture changed; the smoke was updated to validate the new model, not to
force the model to fit the old test shape.

Validated
---------
make clean && make -j$(nproc)
./myos_input --smoke strict_handles   -> 37 PASS, 0 FAIL, 0 WARN
./myos_input --smoke all              -> 1164 PASS, 0 FAIL, 0 WARN

Notes / next obvious work
-------------------------
- The 24-bit slot model is now in place without reserving 16M entries per PID.
- This is still a myOS handle encoding, not a full NT handle-table clone.
- Future work can deepen it with stronger sequence/cookie bits, handle table
  quota accounting, inheritance flags per entry, protection-from-close, and
  eventually kernel/CSR-style duplication semantics.
