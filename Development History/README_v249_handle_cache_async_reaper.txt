# v249 - Handle source-cache validation + async ProcessHost reaper

v249 is a targeted hotpath pass after v248.

## Handle table / DuplicateHandle hotpath

The v242 TLS handle cache still used the per-process handle-table epoch as its
validity guard.  That was conservative, but it meant every CloseHandle() in the
same process invalidated a hot cached source handle, even when the close was for
an unrelated duplicate target.

v249 changes the cache to validate the cached handle's own slot/generation/object
tuple directly:

- the table epoch is still bumped for diagnostics and broad mutation accounting;
- a TLS cache hit now checks the cached slot, generation, handle value, object
  handle, type and object slot;
- closing an unrelated handle no longer evicts the source handle used by repeated
  DuplicateHandle()/CloseHandle() churn;
- stale cache entries are detected when the cached slot is closed or reused.

The smoke adds `v249 source handle cache survives unrelated CloseHandle`, which
runs 2048 duplicate/close cycles and verifies that the hot source handle remains
validated from the TLS cache.

## ProcessHost async reaper

ProcessHost now starts a small detached reaper thread when Linux-backed children
are tracked.  The reaper collects running ProcessHost PIDs, performs
waitpid(WNOHANG), and notifies the KERNEL32 dispatcher through
`MyWinNotifyProcessHostExit()` when a child exits.

This removes the need for process/thread wait paths to add a 25ms polling
deadline when the async reaper is active.  Process/thread WaitBlocks can now be
woken by the ProcessHost exit notification instead of relying only on timed
poll-slices.

New ProcessHost audit counters:

- async_reaper_started
- async_reaper_polls
- async_reaper_reaps
- async_reaper_notifications

The apphost smoke detail now prints these values; a successful run should show
`async=1` plus non-zero reaps/notifications after OOP child cleanup.
