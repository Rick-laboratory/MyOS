# v250 - Handle/Namespace/WFMO Hotpath Pass

This build continues the v249 O(1)/hotpath series with four targeted changes.

## 1. Multi-entry TLS public HANDLE cache

The previous TLS handle cache was effectively optimized for one stable source
handle. v250 makes it a small direct-mapped cache so paths that bounce between a
few public handles (notably WaitForMultipleObjects and mixed USER/KERNEL loops)
can keep more than one resolved handle hot.

Diagnostics added:

- `handle_cache_slot_probes`
- `handle_cache_slot_collisions`

The smoke test `v250 WFMO prevalidation/cache benchmark` exercises four handles
in a repeated WFMO timeout loop and expects the handles to stay hot after the
first round.

## 2. Per-process handle free-stack mark bits + TLS free-slot hint

Handle leaf pages now carry one free-stack mark bit per slot.  Closing a handle
marks and pushes a free slot only once, so duplicate free-stack entries are
prevented in O(1).  The same thread also records the freshly closed slot as a
TLS free hint.  The next same-thread handle allocation can consume that top slot
without walking the global free stack.

Diagnostics added:

- `handle_free_hint_hits`
- `handle_free_hint_misses`
- `handle_free_mark_duplicate_skips`
- `handle_free_stale_pops`

The smoke test `v250 handle free-slot TLS hint reuses close->alloc slots` checks
that DuplicateHandle/CloseHandle churn actually reuses the freshly closed slot.

## 3. WFMO prevalidation and immediate process fast path

`WaitForMultipleObjects()` now resolves and validates the public handles once at
the top of the call, then carries the resolved object handles/types through the
probe and commit phases.  This avoids repeated public-handle table resolution in
the WFMO loop.

For targeted WFMO sets, WaitBlock registration is now delayed until the first
blocking wait even when the set contains a process/thread object.  Already
signaled process/thread handles can therefore complete immediately without
linking WaitBlocks.

Diagnostics added:

- `wait_multiple_prevalidated`
- `wait_multiple_prevalidate_resolves`
- `wait_multiple_prevalidate_fallbacks`

The smoke test `v250 targeted WFMO immediate process fast path skips WaitBlocks`
checks that an already-exited process in a WFMO set returns immediately with no
WaitBlock link/deferred registration.

## 4. Global named-object directory preflight

Events, Mutexes, Semaphores, Sections and Waitable Timers now share a small
central named-object directory in addition to their typed hash tables.  The typed
hashes remain the fast payload lookup, while the shared directory rejects
cross-type collisions such as creating a Mutex with an existing Event name.

The smoke test `v250 named directory rejects cross-type collision` covers the
cross-type namespace rule and verifies that the name is reusable after the old
object is closed and removed.

## Validation

Final validation for this package:

```text
make clean && make -j2
./myos_input --smoke all
./myos_input --smoke wait_real
```

Both smoke runs pass with 0 failures.
