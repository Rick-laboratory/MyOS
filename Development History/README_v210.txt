myOS v210 - per-process handle-table locks + quota accounting

Goal
----
Move the v205 sparse 24-bit handle table away from one global table mutex on the hot path.
The global handle lock now protects the process-table directory/audit metadata; individual process
handle tables have their own pthread mutex. This is closer to NT ObjectTable granularity and prevents
process A handle churn from blocking process B handle operations.

Changes
-------
- MyWinHandlePidTable now owns a per-process pthread_mutex_t lock.
- Handle allocate/resolve/close/duplicate-to-pid hot paths use the owning process table lock.
- The process table directory remains protected by g_HandleLock.
- Empty handle tables are retained after close to avoid racing per-table lock lifetime; process-exit sweep
  still owns full table cleanup semantics.
- Added per-table quota/peak/quota_failures fields with a default quota of 1,048,576 handles per process.
  This is intentionally far below the 24-bit address space and acts as the future anti-DoS accounting point.

Notes
-----
This is not a fake lock-free rewrite. It keeps the NT-shaped sparse table and simply moves contention
granularity from global to per-process, which is the correct next step before any push-lock/cache work.
