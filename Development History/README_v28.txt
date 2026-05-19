myOS v28 - Process Objects + Process-local Handle Tables

Goal:
  v27 had global object ids that behaved like HANDLEs.
  v28 introduces the more NT-like split:

    app / pid
      -> process-local HANDLE value
      -> handle table entry
      -> global Object Manager object handle
      -> object state / refcount / name

What changed:
  - CreateEventA / OpenEventA / CreateFileMappingA / OpenFileMappingA now return process-local HANDLE values.
  - DuplicateHandle now creates a new handle table entry, so the target HANDLE value is visibly different.
  - CloseHandle removes the current process handle entry, then releases the referenced global object.
  - WaitForSingleObject / WaitForMultipleObjects / SetEvent / ResetEvent / MapViewOfFile resolve HANDLE -> object internally.
  - MyWinBindRuntime exposes app/capability ids as PROCESS objects in ObjectLab.
  - ObjectLab now shows two tables:
      1. GLOBAL OBJECTS
      2. PROCESS HANDLE TABLES: PID HANDLE -> OBJ

Useful test:
  1. Open ObjectLab and WaitLab.
  2. Click WaitLab: Create Event a few times.
  3. Refresh ObjectLab.
     You should see EVENT rows in GLOBAL OBJECTS and corresponding process HANDLE rows.
  4. Click Duplicate in WaitLab.
     The duplicate HANDLE should be different, while OBJ points to the same EVENT object.
  5. Click Set/Reset/Wait.
     The wait APIs still work through the process-local handles.
  6. Click Close All.
     Handle entries disappear first; global objects disappear when the refcount reaches zero.

Still deliberately not finished:
  - No real address-space isolation yet.
  - DuplicateHandle still targets the current process in this PoC; cross-process target tables are the next step.
  - Handle inheritance on CreateProcess-lite is not wired yet.
  - Access checks are recorded as granted_access but not enforced deeply yet.
