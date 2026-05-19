myOS v187 - waitable ref/signal audit

Focus:
- Waitable object semantics after v186 DuplicateHandle/inherit hardening.
- Wait diagnostics distinguish success/timeout/failure, wait-any/wait-all commits,
  and consumptions of event/semaphore/timer/mutex objects.
- Mutex owner process exit now marks owned mutexes as abandoned before sweeping the
  exiting process handle table, so remaining duplicated/open handles see
  WAIT_ABANDONED and acquire the mutex.
- Auto-reset event duplicate handles now have smoke coverage proving one signal is
  consumed once even when observed through two process handles.
- AppHost DialogLab smoke waits for real parent-created batch child HWNDs before
  asserting tab order, avoiding request-side timing false negatives.

Smoke highlights:
- wait_real auto-reset duplicated handles consume once
- wait_real mutex abandoned on owner process exit
- wait_real waitable audit counters reflect real commits
- apphost OOP DialogLab tab order wraps through real batch HWNDs
