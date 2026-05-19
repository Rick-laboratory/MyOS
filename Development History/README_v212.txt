myOS v212 - EX-pushlock-style handle tables + multiprocess handle churn

Focus:
- Replace the v211 pthread_rwlock_t pushlock-lite wrapper with a real userspace EX_PUSH_LOCK-style primitive for handle-table hot paths.
- Add a multiprocess-parallel handle churn smoke/benchmark so v210/v211/v212 changes are measured across separate per-process handle tables, not only same-process contention.

Changes:
- MyWinPushLock now uses a compact atomic state word:
  - state == 0: unlocked
  - bit 0: exclusive owner present
  - bits 31..2: shared reader count
- Uncontended shared/exclusive acquire uses a CAS fast path.
- Contended paths park on pthread_cond_t waiters.
- Writer waiters are tracked separately so new readers stop barging while a writer is queued.
- Handle-table resolve paths still use shared acquire.
- Allocation/close/metadata mutation still use exclusive acquire.
- Added pushlock audit counters to MyHandleTableAudit:
  - pushlock_shared_fast
  - pushlock_shared_slow
  - pushlock_exclusive_fast
  - pushlock_exclusive_slow
  - pushlock_wakeups
  - pushlock_contentions
- Added v212 multi-process handle churn benchmark:
  - 4 distinct runtime PIDs/capabilities
  - each thread owns its own process-local handle table
  - each creates its own event
  - each performs duplicate+close churn in parallel
  - verifies count_before == count_after per PID

Notes:
- This is not Windows kernel EX_PUSH_LOCK code; that implementation is private NT kernel code.
- It is the correct myOS/Linux-userspace equivalent shape: atomic fast path, tiny state word, parked waiters only on contention, shared readers, exclusive writers.

Validation:
- make clean && make -j$(nproc)
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all
