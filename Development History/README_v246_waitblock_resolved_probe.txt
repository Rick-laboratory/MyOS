myOS / Linux-Win32 v246 -- WFMO WaitBlocks + resolved wait probes
==================================================================

Focus
-----
This pass continues the v244/v245 dispatcher cleanup.  v244 moved
WaitForSingleObject(Event/Mutex/Semaphore/Timer) onto object-local condvars.
v245 made pure native WaitForMultipleObjects() targeted, but the multi-waiter
registry still lived as a central dispatcher list.  v246 moves native WFMO
waiters closer to NT dispatcher-object semantics.

Changes
-------

1. Per-object WFMO WaitBlocks
   - Event/Mutex/Semaphore/Timer objects now carry a waitHead list.
   - WaitForMultipleObjects() creates one stack MyWinMultiWaiter and one stack
     MyWinWaitBlock per waited object.
   - Each WaitBlock is linked directly to the target object's waitHead.
   - SetEvent/ReleaseMutex/ReleaseSemaphore/SetWaitableTimer/CancelWaitableTimer
     wake only the WaitBlocks attached to the changed object.
   - Process/thread handles remain on the global fallback path because their
     current exit wakeups still come from ProcessHost/Object polling.

2. Object teardown safety for wait lists
   - Release paths detach any outstanding WaitBlocks from an object before the
     object slot is cleared/destroyed.
   - Detach wakes registered waiters so they can re-probe and fail/timeout using
     the existing conservative wait validation semantics.

3. Resolved-object WFMO probes
   - The targeted WFMO path already resolves public handles to dispatcher object
     handles/types before registration.
   - v246 reuses those resolved (object,type) pairs for scan/commit probes.
   - This avoids repeating public handle-table resolution inside every
     WAIT_ANY/WAIT_ALL scan iteration for pure native waitable-object sets.

4. Diagnostics and smoke coverage
   - MyWaitAudit now reports WaitBlock links/unlinks/object wakes and resolved
     probe count.
   - wait_real adds a v246 fanout test with multiple simultaneous WFMO waiters.
   - The final audit line exposes objectWake/link/resolvedProbe counters.

Validation
----------

Built with:
    make clean && make -j2

Smoked with:
    ./myos_input --smoke all

Expected final result:
    SMOKE RESULT: PASS (0 failures)

Representative wait_real lines from the final run:
    [PASS] wait_real v246 targeted WFMO per-object WaitBlock fanout :: waiters=6 wakes=6 objectWakes=6 links=12 resolvedProbes=30 prev=0
    [PASS] wait_real waitable audit counters reflect real commits and targeted waits :: ... multiWake=9 objectWake=9 links=18 resolvedProbe=69 bcast=0 skip=37 ...

Notes
-----

This is still not a complete NT dispatcher implementation.  It is, however,
closer than the v245 central waiter registry: native WFMO waiters are now linked
by object, and state transitions no longer need to search an unrelated global
multi-waiter list.  The remaining dispatcher work is to fold Process/Thread
objects into the same dispatcher-object model and eventually converge the
single-wait and multi-wait paths around one DispatcherHeader/WaitBlock design.
