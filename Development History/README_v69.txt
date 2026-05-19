myOS v70 - Kernel Bridge Lockfix + MSDN Mouse cleanup
======================================================

Base: v68_msdn_mouse_kernel_bridge

Goal
----
v68 introduced the child -> parent Kernel/Object syscall bridge. The concept was
right, but KREQ execution happened from the ProcessHost IPC drain path while
`g_ProcessHostLock` was held. Blocking waits could therefore stall the whole
ProcessHost path and deadlock cross-process SetEvent/Wait scenarios.

v70 makes the bridge safe enough to become the base for OOP WaitLab/ObjectLab in
v70+.

What changed
------------

1) Kernel Bridge no longer executes Object/Wait APIs under g_ProcessHostLock
   - `processhost.c` now snapshots KREQ fields under the ProcessHost lock.
   - Actual Create/Open/Set/Reset/Close/Wait execution runs in a detached worker.
   - The worker enters the requesting child myOS process context via
     `MyWinEnterProcessContext(childPid)`.
   - ACK publishing reacquires `g_ProcessHostLock` only after the syscall body is
     done.

2) Blocking waits no longer block the ProcessHost drain path
   - `WaitForSingleObject(INFINITE)` and `WaitForMultipleObjects(INFINITE)` from
     an OOP child now wait in a worker thread.
   - Child B can still submit `SetEvent` while Child A is waiting.
   - Wait workers use short wait slices and re-check whether the original KREQ
     sequence is still current, so stale/time-out requests do not overwrite a
     newer child request.

3) ACK sequence hardening
   - The parent captures `kernel_request` into the KREQ job.
   - Completion ACK writes `kernel_ack = captured_seq`, not "whatever the shared
     field currently contains".
   - If the child has already submitted a newer request, the old result is
     treated as stale and not published over the new request.

4) Child-side INFINITE wait is no longer a 1500 ms lie
   - `child_kernel_submit()` accepts an infinite ACK wait budget for true
     INFINITE waits.
   - Finite waits use `dwMilliseconds + 1000 ms` as ACK grace instead of the old
     hard cap.

5) Runtime process context made thread-local
   - The current myOS capability/runtime stack in `mywin.c` is now `__thread`.
   - This prevents KREQ worker threads from corrupting the UI/main thread's
     current process context while they temporarily enter a child process.

6) Mouse cleanup from v68
   - Removed dead refactor residue from `MyButtonWndProc` where the same
     GET_X_LPARAM/GET_Y_LPARAM values were recomputed in an impossible branch.
   - MSDN mouse convention from v68 remains intact:
     `lParam = MAKELPARAM(x,y)`, `wParam = MK_*` modifier/button flags.

7) Documentation
   - `LOCK_ORDER.md` updated to v69.
   - New explicit rule: ProcessHost may copy KREQ metadata under lock, but must
     never execute Object Manager or Wait APIs while holding `g_ProcessHostLock`.

Build
-----

    make clean && make

Expected result:

    BUILD: myos_v70_oop_wait_objectlab_duplicatehandle
    gebaut: ./myos_input
    gebaut: ./myos_apphost_child

Known remaining limitations
---------------------------

- The underlying WaitForMultipleObjects implementation is still polling-based in
  `mywin.c`; v70 fixes bridge lock-safety, not the Dispatcher core itself.
- The bridge is still Event-focused for the shipped child selftest, although the
  worker path also covers Mutex/Semaphore/Close/Wait ops already present in v68.
- Full OOP migration of ObjectLab/WaitLab/SectionLab is intentionally left for a
  follow-up version, now that the bridge is safer.
