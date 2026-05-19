BUILD: myos_v149_user32_wmtimer_queue_v1

Scope
-----
v149 adds USER32 SetTimer/KillTimer and WM_TIMER message-pump synthesis on top
of v147 strict public handles and v148 dispatcher-condvar waits.

Implemented
-----------
- Public SDK declarations for TIMERPROC, SetTimer and KillTimer in winuser.h.
- USER32 exports through GetProcAddress/import registry:
  - SetTimer
  - KillTimer
- Queue-owned USER32 timer metadata on MyThreadQueue.
- HWND timers: SetTimer(hwnd, id, elapse, NULL) synthesizes WM_TIMER for the
  owning window queue.
- Thread timers: SetTimer(NULL, 0, elapse, NULL) generates a thread-local timer
  id and synthesizes WM_TIMER with hwnd == NULL.
- TIMERPROC path: DispatchMessageA calls the callback when WM_TIMER.lParam
  carries a timer procedure pointer.
- Low-priority WM_TIMER behavior: GetMessageA/PeekMessageA check real queued
  messages first and synthesize a due timer only when no matching queued message
  is available.
- PM_NOREMOVE observes a due timer without advancing/consuming it; PM_REMOVE and
  GetMessage advance the timer.
- Drift-aware rescheduling: timers advance by period and skip missed periods
  phase-aligned instead of anchoring every next_due to now.
- KillTimer removes pending timer metadata and wakes the queue.
- DestroyWindow prunes timers owned by that HWND.
- MyMessageQueue condition variable now uses CLOCK_MONOTONIC to match USER32
  timer deadlines.
- LOCK_ORDER.md updated with the v148 Dispatcher lock and v149 USER32 timer
  queue wake rule.

Smoke
-----
New group:

  ./myos_input --smoke user32_timer

Validated cases:
- SetTimer(hwnd,id,NULL) returns the caller id.
- WM_TIMER is not delivered before due time.
- Queued WM_COMMAND wins over a due synthetic WM_TIMER.
- PeekMessageA(PM_NOREMOVE) observes WM_TIMER without consuming it.
- PeekMessageA(PM_REMOVE) consumes WM_TIMER.
- DispatchMessageA routes NULL TIMERPROC timers to the owner WndProc.
- DispatchMessageA calls TIMERPROC for callback timers.
- SetTimer(NULL,0,...) generates a thread timer id and delivers hwnd == NULL.
- KillTimer removes both window and thread timers.
- A killed timer remains silent after its original due time.

Regression results
------------------
make clean && make -j2
  PASS

./myos_input --smoke user32_timer
  PASS: 18/18

./myos_input --smoke strict_handles
  PASS: 36/36

./myos_input --smoke wait_real
  PASS: 15/15

./myos_input --smoke all
  PASS: 658 PASS, 0 FAIL, 0 WARN

Notes / remaining gaps
----------------------
- v149 is USER32 timer work only. It intentionally does not merge SetTimer with
  KERNEL32 CreateWaitableTimer/WaitForSingleObject; those remain separate
  Win32 subsystems.
- The current PoC still treats pid == tid for one UI-thread queue per capability.
  Future real thread IDs should move timers to real per-thread queues without
  changing the public SetTimer/KillTimer contract.
- Full Win32 timer details such as USER_TIMER_MINIMUM/USER_TIMER_MAXIMUM clamp
  constants and all edge-case LastError behavior are still future conformance
  polish.
