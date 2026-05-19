# v250 – WFMO prevalidation + multi-entry TLS handle cache

Focus: keep the post-v249 hot paths moving toward 100% without risking the
WaitBlock/DispatcherHeader semantics that v244-v249 established.

## Changes

- Replaced the one-entry per-thread public HANDLE lookup cache with a small
  direct-mapped 16-slot TLS cache.  Each hit is still validated against the
  concrete handle-table slot, generation, object handle, type and object slot;
  unrelated `CloseHandle()` calls continue to bump the table epoch for audit,
  but they do not evict stable entries.

- Added diagnostics for TLS handle-cache slot probes and slot collisions.

- Collapsed `WaitForMultipleObjects()` validation and target-resolution into
  one pass.  The old shape resolved each public handle once during validation
  and again while deciding whether the wait set could use the targeted
  dispatcher path.  v250 fills the resolved `(objectHandle, objectType)` arrays
  during validation and reuses them for targeted waits, immediate probes and
  WaitBlock linking.

- Added wait diagnostics for WFMO prevalidation count, resolved-handle count
  and legacy fallback count.

- Kept the v248/v249 immediate WFMO process path: targeted process/thread wait
  sets delay WaitBlock linking until after the first immediate probe.  The
  WAIT_ANY immediate process path now also records the process/thread immediate
  counter.

- Added/kept build-safety prototypes for the new named-directory helpers so the
  central named-object directory path compiles cleanly under `-Wall -Wextra`.

## Smoke coverage

New strict-handles coverage exercises four stable waitable handles through
`WaitForMultipleObjects(..., FALSE, 0)` in a tight loop.  Expected behavior:

- exactly one prevalidation/resolve pass per handle per call,
- multi-entry TLS cache hits for the stable wait set after the initial miss,
- cache-slot probes/collisions are now visible in the audit line.

Representative line:

```
[PASS] strict_handles v250 WFMO prevalidation and multi-entry handle TLS cache
```

Full validation:

```
make clean && make -j2
./myos_input --smoke all
SMOKE RESULT: PASS (0 failures)
```
