# v248 WFMO immediate fast path

v248 tightens the targeted WaitForMultipleObjects hot path after v246/v247.

Changes:
- Native Event/Mutex/Semaphore/Timer WFMO sets now probe/commit once before
  attaching stack WaitBlocks. Already-signaled WAIT_ANY/WAIT_ALL calls avoid
  pthread_cond_init/destroy and intrusive list link/unlink work.
- PROCESS/THREAD waits still register eagerly after an immediate exited-object
  probe, preserving the current try-lock wake safety for process teardown.
- New WaitAudit counters track immediate targeted WFMO hits, deferred native
  WaitBlock registration, and immediate process/thread hits.
- Smoke adds a v248 immediate native WFMO test proving an already-signaled
  native object returns with zero new WaitBlock links.

This is intentionally a hot-path cleanup rather than a semantic expansion: the
v246/v247 object-linked WaitBlock model remains intact for real blocking waits.
- Multi-wait gate condvars are now per-thread reusable TLS condvars. Blocking
  WFMO/process-thread waits no longer initialize and destroy a pthread condvar
  for every call; the intrusive WaitBlocks stay stack-local while the wait gate
  is reused by the calling thread.

- Event/Mutex/Semaphore/Timer and Process/Thread wait targets now carry a small
  DispatcherHeader with type, object handle, signal state and waitHead. This is
  a staged move toward a single NT-like dispatcher-object layout instead of
  per-object ad-hoc waitHead fields.
- Fixed kernel table free stacks now keep O(1) byte marks, so duplicate-free
  protection no longer scans the whole free stack on every release.
