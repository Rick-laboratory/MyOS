# v247 - Process/Thread WaitBlocks

This pass extends the v246 object-linked WaitBlock model to process and thread
handles.

## What changed

- Process/thread wait heads were added to the lightweight process record:
  `processWaitHead` and `threadWaitHead`.
- `WaitForSingleObject()` can now park on a process/thread object-linked
  WaitBlock instead of going through the global dispatcher fallback.
- `WaitForMultipleObjects()` can now use the targeted WaitBlock path for sets
  that include process/thread handles together with native waitables.
- Process exit now wakes the specific process and thread dispatcher objects via
  object-local WaitBlock signaling instead of relying on the global dispatcher
  broadcast path.
- The WFMO targeted path reuses resolved object handles/types for
  process/thread wait probes, avoiding repeated public handle resolution inside
  the wait loop.
- New wait-audit counters expose process/thread targeted waits, object-local
  wakeups, and poll slices:
  - `wait_process_thread_targeted`
  - `wait_process_thread_poll_slices`
  - `wait_process_thread_object_wakes`

## Why it matters

v246 already moved native Event/Mutex/Semaphore/Timer WFMO waits from a central
registry to per-object WaitBlocks. Process/thread waits were still the largest
remaining dispatcher fallback. v247 pulls those handles into the same targeted
model, so process exit can wake only waiters that actually wait on that process
or thread object.

Linux-backed AppHost/ProcessHost exit is still polled in short slices while the
waiter is asleep, because this userspace layer does not yet have a fully async
kernel-style process-exit callback. The important structural change is that the
waiter is now object-linked and wake-targeted, not globally broadcast.

## Validation

Final build:

```text
make clean && make -j2
```

Final smoke:

```text
./myos_input --smoke all
SMOKE RESULT: PASS (0 failures)
```

Key smoke lines:

```text
[PASS] wait_real v247 targeted WaitForSingleObject(process) WaitBlock :: result=0x0 elapsed_ms=15 worker=1 exit=247 ptTarget=1 ptWake=1
[PASS] wait_real v247 targeted WaitForMultipleObjects(process) WaitBlock :: result=0x1 elapsed_ms=16 worker=1 ptTarget=1 ptWake=1 links=2
[PASS] wait_real waitable audit counters reflect real commits and targeted waits :: ... ptTarget=2 ptPoll=2 ptWake=2 bcast=0 ...
```
