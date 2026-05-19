# myOS / Linux-Win32 v244 - Targeted Waits + Root Window Hash/List Hotpath

v244 continues the O(1)/cache-quality work after v243.  The patch keeps the
v243 WindowInfo hot/cold split and adds two focused hotpath improvements:

## KERNEL32 wait path

- `WaitForSingleObject()` now has a targeted per-object path for Events,
  Mutexes, Semaphores and Waitable Timers.
- Single-object waits park on the object's own condition variable instead of
  always registering as a global dispatcher waiter.
- The global dispatcher path is retained for process/thread handles and for
  `WaitForMultipleObjects()`/mixed waits.
- Waitable object condition variables use the monotonic clock, matching the
  dispatcher condvar and avoiding realtime-clock drift for timed waits.
- Dispatcher wakeups now skip the global broadcast when no global waiter is
  parked; object-specific condvars are still signaled by SetEvent,
  ReleaseMutex, ReleaseSemaphore and Set/CancelWaitableTimer.
- `MyWaitAudit` exposes `wake_skips`, `wait_single_targeted` and
  `wait_single_global_fallback` to keep the new route visible in smoke.

## USER32 root-window resolve path

- Top-level windows (`parent == NULL`) now use the same intrusive list model
  as child windows: `g_RootFirstWindow` / `g_RootLastWindow`.
- `GetTopWindow(0)` and `FindWindowA()` can avoid legacy whole-table scans in
  the normal root-window case.
- `MyWinWindowInfo` now stores hot `classNameHash` and `textHash` fields so
  FindWindow/FindWindowEx reject non-matches before touching cold string
  buffers.
- LISTBOX/COMBOBOX insert/delete payload shifts use bulk `memmove()` instead
  of per-item loops.

## Validation

- `make clean && make -j2`
- `./myos_input --smoke all`

Expected: `SMOKE RESULT: PASS (0 failures)`.
