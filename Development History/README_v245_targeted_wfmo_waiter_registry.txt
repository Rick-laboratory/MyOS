# v245 targeted WFMO waiter registry

This pass moves the next dispatcher hot path after v244's targeted
WaitForSingleObject work:

- WaitForMultipleObjects for native waitable objects (Event, Mutex,
  Semaphore, Waitable Timer) now registers a small stack waiter on the
  dispatcher mutex instead of parking on the global dispatcher condvar.
- Event/Mutex/Semaphore/Timer state transitions signal only matching
  multi-waiters by resolved object handle.  Process/Thread waits keep the
  global fallback because they are still driven by ProcessHost/Object state.
- The global dispatcher condvar now remains quiet for pure native WFMO waits;
  the wait audit exposes multi-targeted call/fallback/wake counters.
- Smoke adds a real blocking WAIT_ANY worker case for the new path.

Final validation target:

    make clean && make -j2
    ./myos_input --smoke all

Expected final smoke signal:

    SMOKE RESULT: PASS (0 failures)
