# v248 - DispatcherHeader + O(1) fixed-table free marks

This pass continues the v246/v247 dispatcher work and turns the object-linked
WaitBlock model into a more NT-shaped dispatcher-object layout.

## DispatcherHeader

Event, Mutex, Semaphore, Timer, Process and Thread wait targets now carry a
small `MyWinDispatcherHeader`:

- object type
- object handle
- current signal-state mirror
- intrusive WaitBlock head

`WaitForMultipleObjects()` still uses stack WaitBlocks, but the WaitBlock list
head is now reached through this common dispatcher header instead of one-off
per-type fields. Process and Thread objects get the same header shape as native
waitable objects.

## Fast non-ready probe

For non-consuming WFMO probe passes, Events and Semaphores can reject the common
not-ready case from the dispatcher header's signal-state mirror before taking
the object mutex. Ready cases still re-check and commit under the real object
lock, so auto-reset Events and Semaphore counts keep their existing semantics.
Timers are deliberately not fast-rejected this way because a timer may become
due as a function of monotonic time.

## Fixed-table free stack mark bits

The small fixed kernel tables (Sections, Views, Events, Mutexes, Semaphores,
Timers and Tokens) no longer deduplicate free-stack pushes by linearly scanning
the stack. Each table now keeps a one-byte mark per slot:

- push: O(1) mark test + push
- pop: O(1) mark clear + validity check

This removes another avoidable O(n) edge from create/close churn paths while
preserving the same slot-reuse behavior.

## Diagnostics

`MyWaitAudit` now exports DispatcherHeader counters:

- `wait_dispatcher_header_inits`
- `wait_dispatcher_header_head_hits`
- `wait_dispatcher_header_state_stores`
- `wait_dispatcher_header_fast_not_ready`

The wait smoke asserts that the dispatcher headers are initialized, used for
WaitBlock head resolution, updated on state changes and exercised by the fast
not-ready path.

## Validation

Validated with:

```text
make clean && make -j2
./myos_input --smoke all
```

Final result:

```text
SMOKE RESULT: PASS (0 failures)
```
