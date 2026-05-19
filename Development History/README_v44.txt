myOS v44 - process-lite lifecycle hardening
==========================================

Build string:
  myos_v44_process_lifecycle_waitable

Goal:
  First real step after v43 USER32 close/lifecycle hardening toward an NT-like
  ProcessManager boundary. This is still not real Linux process isolation yet,
  but Process-Lite now behaves more like a waitable Win32 process object.

Core changes:
  - CreateProcessA now creates both PROCESS and THREAD object-manager entries.
  - PROCESS_INFORMATION now returns hProcess, hThread, dwProcessId, dwThreadId.
  - Process-Lite keeps PID, PPID, TID, process object, thread object, flags,
    exit code, inherited-handle count, duplicated-in count and live handle count.
  - PROCESS and THREAD objects are now waitable by WaitForSingleObject and
    WaitForMultipleObjects.
  - Added TerminateProcess(hProcess, exitCode).
  - Added GetExitCodeProcess(hProcess, &exitCode) with STILL_ACTIVE semantics.
  - Added MyEnumProcessLite(...) for ProcessManager-style diagnostics.
  - TerminateProcess marks PROCESS/THREAD object info as exited and tears down
    the target PID's local handle-table entries.
  - DuplicateHandle now supports THREAD handles too.

WaitLab additions:
  - Spawn Child now displays real hProcess/hThread + PID/TID.
  - WaitProc waits on the child process handle: live child => WAIT_TIMEOUT.
  - TermProc terminates child with exit code 44.
  - After TermProc, WaitProc returns WAIT_OBJECT_0 and GetExitCodeProcess shows 44.
  - ObjectLab/handle table views now show PROCESS and THREAD object entries.

Still not implemented:
  - No real Linux fork/exec app isolation yet.
  - No real per-process address-space boundary yet.
  - No scheduler-owned thread object beyond the main Process-Lite thread record.
  - No process image loader yet.

Smoke test performed:
  - gcc full build with -Wall -Wextra -O2.
  - Dedicated Process-Lite unit smoke:
      CreateEvent(inheritable)
      CreateProcessA(inherit=TRUE)
      WaitForSingleObject(hProcess, 0) == WAIT_TIMEOUT while live
      TerminateProcess(hProcess, 44)
      GetExitCodeProcess(hProcess) == 44
      WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0 after exit
      child handle table count drops to 0 after termination
