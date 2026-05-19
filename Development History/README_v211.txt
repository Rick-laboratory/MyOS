myOS v211 - pushlock-lite handle tables + TLS last-handle cache

Build base: v210_per_process_handle_locks_quota

What changed:
- Replaced per-process handle-table pthread_mutex_t with MyWinPushLock, a small
  pushlock-lite wrapper over pthread_rwlock_t.
- Handle resolves now acquire the process handle table in shared/read mode.
- Handle allocation/close/SetHandleInformation and exit sweep acquire exclusive/write mode.
- Added conservative thread-local last-handle lookup cache for public handle resolve.
- Cache is guarded by a global handle lookup epoch. Any handle-table mutation
  invalidates every thread cache, so CloseHandle in another thread cannot leave
  stale cached object handles behind.
- Added handle cache audit counters to MyHandleTableAudit:
  handle_cache_hits, handle_cache_misses, handle_cache_stores,
  handle_cache_invalidations.
- strict_handles smoke now includes a v211 repeated same-handle lookup benchmark.

Design notes:
- This is intentionally not a lock-free handle table. NT uses heavily optimized
  lock-based tables; v211 follows that direction with shared/exclusive hotpath
  granularity first.
- Directory/audit enumeration still uses the global directory lock for table-list
  stability. The hot per-process table path is pushlock-lite.
- The TLS cache never owns object lifetime. It is only a cached translation of
  an already-valid handle table entry and is invalidated by the epoch on any
  table mutation.
