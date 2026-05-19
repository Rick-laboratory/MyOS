myOS v29 - CreateProcess-lite + inheritable/cross-process handles
================================================================

This build extends v28's process-local handle table into the next NT-shaped step:

New API surface:
  - OpenProcess(...)
  - CreateProcessA(...)
  - DuplicateHandle(sourceProcess, sourceHandle, targetProcess, ...)
  - PROCESS_INFORMATION / STARTUPINFOA lite structs
  - MyGetProcessLiteInfo(...) debug helper

What is implemented:
  - PROCESS objects are registered in the Object Manager for CreateProcess-lite children.
  - Parent receives a process-local hProcess handle.
  - Handles marked bInheritHandle=TRUE are copied into the child PID's handle table when bInheritHandles=TRUE.
  - DuplicateHandle can now allocate the duplicate handle into another process handle table.
  - ObjectLab shows the result because the child PID gets its own HANDLE -> OBJECT rows.

WaitLab additions:
  - Spawn Child
      Creates an inheritable duplicate of the current Event and calls CreateProcessA(..., inherit=TRUE).
      ObjectLab should show a PROCESS object named waitlab-child-lite and a child PID handle table row pointing at the same EVENT.

  - Dup->Child
      Uses DuplicateHandle(GetCurrentProcess(), currentEvent, hChildProcess, ...)
      to allocate another EVENT handle directly inside the child PID handle table.

Still intentionally lite:
  - No real address-space/process isolation yet.
  - Child process is a PROCESS object + handle table, not an independently scheduled app image.
  - hThread is currently 0; THREAD object model comes later.
  - Security descriptors/ACLs still remain future work.
