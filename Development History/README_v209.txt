myOS v209 - handle contention benchmark / lifetime ref serialization

Built on v208 std handles + single-thread handle benchmark.

Goals
-----
- Keep the v208 single-thread DuplicateHandle/CloseHandle benchmark.
- Add smoke-time multi-thread handle benchmarks so sparse 24-bit handle tables
  have a first contention baseline.
- Do not let the smoke dictate architecture: the benchmark exercises the
  existing Win32-like process-local HANDLE model and reports INFO telemetry.
- Harden object lifetime refcount mutation before exercising DuplicateHandle
  and CloseHandle from multiple pthread workers.

Changes
-------
- Added g_ObjectLifetimeLock around mywin_add_object_ref_by_type() and
  mywin_release_object_ref_by_type().
  This serializes per-object refCount mutations and destruction against
  multi-thread DuplicateHandle/CloseHandle churn.
- CloseHandle() now resolves/removes the public process HANDLE table entry and
  then uses the same central mywin_release_object_ref_by_type() path as exit
  sweep / DuplicateHandle(DUPLICATE_CLOSE_SOURCE) cleanup.
- Added strict_handles v209 fanout benchmark:
  - 4 pthread workers
  - each duplicates 1024 handles from one source event
  - then closes all of them
  - checks the global handle count returns to baseline
- Added strict_handles v209 churn benchmark:
  - 4 pthread workers
  - each performs 2048 DuplicateHandle+CloseHandle operations
  - checks the global handle count returns to baseline
- Worker threads explicitly bind the smoke runtime capability with
  MyWinBindRuntime(), because runtime capability is TLS and real threads do not
  inherit it magically.

Validation
----------
make clean && make -j$(nproc)
./myos_input --smoke strict_handles
./myos_input --smoke all

Observed in this environment
----------------------------
strict_handles: 73 PASS, 0 FAIL, 0 WARN
all:            PASS, 0 FAIL

Example INFO lines from --smoke all:
[INFO] strict_handles v208 handle benchmark :: handles=4096 dup_ms=1.648 close_ms=0.641 dup_ops_s=2485437 close_ops_s=6390016 count_after=1
[INFO] strict_handles v209 handle fanout benchmark :: threads=4 handles=4096 wall_ms=7.215 dup_thread_ms_sum=10.720 close_thread_ms_sum=6.998 ops_s=567706 count_before=1 count_after=1
[INFO] strict_handles v209 handle churn benchmark :: threads=4 operations=8192 wall_ms=8.419 ops_s=973037 count_before=1 count_after=1

Notes
-----
These are intentionally smoke-level baselines, not a formal benchmark suite.
Numbers depend on host, scheduling, compiler, and current lock granularity.
The important pass/fail contract is no leaks, no invalid-handle corruption, and
stable behavior while multiple threads hit the same process-local sparse table.
