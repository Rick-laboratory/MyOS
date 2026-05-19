# v244 targeted wait dispatcher / per-object single waits

This pass moves the high-frequency `WaitForSingleObject()` paths for native
waitable kernel objects away from the global dispatcher condvar and onto the
object-local condition variables that already exist on events, mutexes,
semaphores, and waitable timers.

## What changed

- `WaitForSingleObject()` now uses a targeted per-object wait path for:
  - Events
  - Mutexes
  - Semaphores
  - Waitable timers
- PROCESS/THREAD waits, and future dispatcher-only waitables, keep the old
  conservative global dispatcher fallback.
- The global dispatcher now tracks parked global waiters. State transitions
  still advance the dispatcher sequence, but they only broadcast the global
  condvar when a real global waiter is blocked.
- Waitable object condvars are initialized with the same monotonic-clock policy
  as the dispatcher condvar, so timed event/semaphore/mutex/timer waits use a
  consistent clock source.
- New wait audit counters expose:
  - `wait_single_targeted`
  - `wait_single_global_fallback`
  - `wake_skips`
- Smoke includes a v244 targeted `WaitForSingleObject(event, 0)` benchmark plus
  an audit assertion that targeted waits and skipped global broadcasts are
  actually exercised.

## Why this matters

The previous dispatcher model was correct but coarse: every supported waitable
state transition used the global dispatcher broadcast path so that multiple
waits could observe it. That is still needed for `WaitForMultipleObjects()` and
process/thread waits, but it is unnecessary work for the common single-object
case.

The v244 path is closer to NT's dispatcher-object idea: the hot single-object
waiter parks on the object that can satisfy it, while the global dispatcher is
reserved for aggregate waits and process/thread polling.

## Validation

- `make clean && make -j2`
- `./myos_input --smoke all`
- Final smoke: `SMOKE RESULT: PASS (0 failures)`

Relevant final smoke lines:

```text
[PASS] wait_real v244 targeted WaitForSingleObject event benchmark :: probes=8192 ok=8192 wall_us=1615 ops_s=5072446
[PASS] wait_real waitable audit counters reflect real commits and targeted waits :: success=12311 timeout=10 any=4 all=3 event=5 sem=8 timer=3 abandoned=1 targeted=12309 fallback=0 bcast=2 skip=27 fail=10
```

## Remaining work

This is deliberately not a full per-object wait-block implementation yet.
`WaitForMultipleObjects()` still uses the global dispatcher lock/condvar for
atomic WAIT_ALL/WAIT_ANY semantics. The next deeper step would be a true
wait-block list per dispatcher object, so signaling can wake only the relevant
wait sets instead of relying on a central multiple-wait rendezvous.
