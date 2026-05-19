# myOS v149 Lock Order

Always acquire locks from top to bottom. Never grab a higher lock while already
holding a lower one. A function named `*_locked` may only copy scalar state or
perform local bookkeeping unless this file explicitly allows more.

1. Desktop / WindowManager state
2. HWNDManager.lock
3. USER32-lite global state (`g_User32LiteLock` where present)
4. GDI lock (`g_GdiLock`)
5. Dispatcher wait lock (`g_DispatcherLock` / `g_DispatcherCond` / `g_DispatcherSeq`)
6. Object Manager lock (`g_ObjLock`) and object-local locks
7. Message queue lock (`MyMessageQueue.lock`, per UI thread)
8. IPC bus / ProcessHost locks (`g_ProcessHostLock`)
9. App-local locks (`g_*.lock`)

## v70 Kernel Bridge rule

The child -> parent Kernel/Object bridge must not execute Object Manager or Wait
APIs while holding `g_ProcessHostLock`.

Allowed under `g_ProcessHostLock`:

- drain/copy one IPC datagram
- copy a KREQ snapshot from the shared section
- update ProcessHost diagnostics
- publish a completed ACK/result

Forbidden under `g_ProcessHostLock`:

- `CreateEventA` / `OpenEventA` / `SetEvent` / `ResetEvent`
- `CreateMutexA` / `ReleaseMutex`
- `CreateSemaphoreA` / `ReleaseSemaphore`
- `WaitForSingleObject` / `WaitForMultipleObjects`
- any future Object Manager / Section / Timer syscall body

v70 runs KREQ bodies in detached worker jobs. The worker switches into the
child's myOS process context with `MyWinEnterProcessContext(childPid)`, executes
without `g_ProcessHostLock`, then reacquires `g_ProcessHostLock` only to publish
the ACK for the exact captured request sequence.

## v148 Dispatcher wait note

`g_DispatcherLock` sits below GDI and above Object Manager/object-local locks.
Wait probing/commit and all signal paths must acquire locks in this order:

`g_DispatcherLock -> object-local lock`

The reverse direction is forbidden.  No wait path may block while holding
Object Manager, HWNDManager, GDI, queue, ProcessHost, or app-local locks.
`g_DispatcherSeq` is currently an audit/diagnostic sequence for future
optimization; correctness comes from broadcast + re-probe under the dispatcher
lock.

## v149 USER32 timer note

USER32 `SetTimer` / `KillTimer` timers are queue-owned metadata, not KERNEL32
waitable timers.  They may be inspected while holding `HWNDManager.lock`, and
changing timer state may wake the associated `MyMessageQueue.wake` condition.
The allowed order is therefore:

`HWNDManager.lock -> MyMessageQueue.lock`

`GetMessageA` / `PeekMessageA` must not hold the queue lock while calling WndProc
or TIMERPROC callbacks.  `WM_TIMER` remains synthetic/low-priority: queued
input/window/normal messages are checked first; only then may the due timer be
materialized.

## Notes

- App WndProc code should avoid calling back into WindowManager while holding an
  app-local lock.
- GDI/Object paths currently take `g_GdiLock -> g_ObjLock`; keep that order.
- `SendMessageTimeout` style sync dispatch is allowed to wait, but the sender
  must not hold an app-local lock that the receiver also needs.
- A wait may block, but it must not hold ProcessHost, HWND, GDI, queue, or
  app-local locks while it blocks.

v71 Section/FileMapping bridge note
-----------------------------------
KREQ_CREATE_FILE_MAPPING / KREQ_OPEN_FILE_MAPPING / KREQ_MAP_VIEW_OF_FILE / KREQ_UNMAP_VIEW_OF_FILE follow the v69 rule:
ProcessHost may copy IPC request metadata while holding g_ProcessHostLock, but Section/Object execution must run in the detached kernel worker outside g_ProcessHostLock.

OOP MapViewOfFile returns mapping metadata, not a parent pointer. The child maps the POSIX shm backing in its own address space.


v72 StateBus note
-----------------
StateBusLab uses the v71 Section/FileMapping bridge and normal PostMessage routing.
The shared payload is protected by a tiny seqlock-style sequence in the mapped
section, not by ProcessHost or HWND locks. Dirty PostMessage/Event signals must
never carry payload pointers; receivers re-read the shared section.

v73 note - HWND shared WindowState section
------------------------------------------
The HWND manager may mirror its in-memory MyWindowStateSection into the v73 named
FileMapping while holding HWNDManager.lock. This mirror copy is raw shared memory
write-only from the parent side and must not call Object Manager, ProcessHost,
WindowManager, or app WndProc callbacks while the HWND lock is held.

v130 USER32 Thread/Window Ownership note
----------------------------------------
USER32 now tracks Parent, Owner, ProcessId and ThreadId separately in the
USER32 window metadata.  Keep these operations lock-light:

- `GetWindowThreadProcessId`, `GetParent`, `GetWindow(GW_OWNER)`, and ownership
  predicates may inspect USER32 metadata and/or copy scalar HWNDManager owner
  fields, but must not dispatch WndProc callbacks while holding HWNDManager.lock.
- Same-thread `SendMessageA` may call the local USER32 WNDPROC directly only
  when the caller owns the target HWND.  Foreign HWNDs must flow through the
  HWND queue/sync transport instead of direct-calling another thread's WndProc.
- Future real cross-thread SendMessage LRESULT propagation must keep this order:
  copy USER32 metadata -> release USER32/HWND locks -> enqueue sync send -> wait
  on SyncSendContext.  Never wait while holding HWNDManager.lock or future
  USER32-global locks.
- `AttachThreadInput` in v130 is a tiny USER32-lite table.  It must remain a
  metadata check only; it must not take queue locks or call WndProc callbacks.

## v131 HWND access-control notes

v131 keeps the same lock ordering as v130 and intentionally avoids holding USER32
metadata locks while calling foreign WndProcs. The new access policy is checked
before message enqueue/direct dispatch:

- Same-owner USER32 HWND: direct WndProc calls remain allowed for the current PoC.
- Foreign USER32 HWND: dangerous messages are rejected before enqueue/dispatch.
- `DispatchMessageA()` no longer treats a manually constructed public `MSG` as
  permission to call a foreign USER32 WndProc.
- Mutating calls (`SetWindowTextA`, `EnableWindow`, `SetWindowLongPtrA`,
  `SetParent`) update `MyWinWindowInfo` only from the owning UI context.

When real cross-thread SendMessage is added, the reply path should be represented
as an explicit per-thread send slot/queue object in this document, including which
API holds which lock while waiting.


v214 Object type-table note
---------------------------
Event, Mutex and Semaphore named-object create/open/lifecycle use separate
per-type locks:

- g_EventTableLock
- g_MutexTableLock
- g_SemaphoreTableLock

These locks protect the fixed type table slot lifecycle and the per-type named
hash buckets. They must not be held while blocking on object-local wait state.
The normal signal/wait order remains:

`g_DispatcherLock -> object-local lock`

Create/Open may briefly take a type-table lock and then Object Manager metadata
locks to register/apply security. Refcount increments on existing objects are
atomic; final zero-ref destruction removes the object from the owning type hash
under the owning type-table lock.
