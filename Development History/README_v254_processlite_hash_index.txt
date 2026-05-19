myOS / Linux-Win32 v254 - ProcessLite PID/TID hash index
=========================================================

Focus
-----
v253 made the named-object directory carry typed payload slots.  v254 moves the
next remaining process-management hot path away from fixed table scans:
ProcessLite records are now indexed by PID and TID through power-of-two hash
buckets, and new ProcessLite slots use an allocation hint instead of scanning
from slot zero every time.

What changed
------------
* MyWinProcessLite now carries PID/TID index metadata:
  - pidHash / tidHash
  - pidHashNext / tidHashNext
  - pidIndexed / tidIndexed

* New ProcessLite index state:
  - g_LiteProcessPidHash[128]
  - g_LiteProcessTidHash[128]
  - g_LiteProcessAllocHint

* Hot lookup helpers now use the index:
  - mywin_find_lite_process_locked(pid)
  - mywin_find_lite_thread_locked(tid)
  - mywin_process_object_for_pid_locked(pid)
  - mywin_thread_object_for_tid_locked(tid)

* Common process paths benefit from the hash-indexed lookups:
  - OpenProcess
  - OpenThread
  - GetExitCodeProcess / mywin_process_is_exited
  - TerminateProcess
  - process/thread pseudo-handle resolution
  - inherited/duplicated handle accounting
  - MyGetProcessLiteInfo

* A conservative repair path remains: if a record is ever found by a fallback
  scan, it is reinserted into the PID/TID index.  Normal runtime lookup is the
  hash path.

Diagnostics / smoke
-------------------
A new diagnostic surface was added:

    MyProcessIndexAudit
    MyWinGetProcessIndexAudit()

The new wait_real smoke creates a process, repeatedly exercises
OpenProcess/OpenThread/GetExitCodeProcess/MyGetProcessLiteInfo, and verifies that
PID/TID lookups hit the hash-indexed path.

Expected smoke lines:

    v254 ProcessLite PID/TID hash index
    v254 ProcessLite hash-index benchmark

Why this matters
----------------
ProcessLite still had several O(MYWIN_MAX_LITE_PROCESSES) PID/TID scans.  The
current table is small, but these functions sit under wait/process/open/pseudo
handle paths.  v254 makes the shape match the rest of the v239-v253 work:
slot-coded objects, power-of-two buckets, O(1)-average lookup, and a fallback
repair path for diagnostics/legacy safety.
