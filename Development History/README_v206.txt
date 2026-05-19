myOS v206 - pseudo handle materialization + sparse handle cleanup
================================================================

Goal
----
Follow v205's sparse 24-bit per-process handle tables with the next Win32
handle-compatibility step: current process/thread pseudo handles are constants,
not normal table entries, but DuplicateHandle must be able to materialize them
into real closeable process/thread handles.

What changed
------------
- Verified the Opus note: mywin_make_user_handle() still accepted pid but only
  did (void)pid after v205 removed PID bits from the public handle encoding.
- Removed the dead pid parameter from mywin_make_user_handle(). The public
  handle encoding now takes only slot + generation.
- Added GetCurrentThread() to the public SDK/header/export surface.
- Added explicit pseudo-handle constants:
  - GetCurrentProcess() -> 0xffffffff
  - GetCurrentThread()  -> 0xfffffffe
- DuplicateHandle now recognizes current-process and current-thread pseudo
  source handles when the source process is the current process and materializes
  them into real per-process handle table entries.
- The materialized duplicate is a normal closeable HANDLE backed by the PROCESS
  or THREAD Object Manager object.
- CloseHandle(GetCurrentThread()) is a no-op success.
- CloseHandle(GetCurrentProcess()) deliberately keeps the invalid-handle path
  because its value is the same bit pattern as INVALID_HANDLE_VALUE in this
  uint32 HANDLE model. DuplicateHandle is the correct path to convert it into a
  real closeable handle.

Smoke updates
-------------
strict_handles now checks:
- pseudo handle constants
- DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), ...) returns a real
  PROCESS handle table entry
- DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), ...) returns a real
  THREAD handle table entry
- CloseHandle current-thread pseudo no-op semantics
- current-process pseudo bitpattern keeps INVALID_HANDLE_VALUE CloseHandle
  behavior while still being duplicable through DuplicateHandle

Validated
---------
make clean && make -j$(nproc)
./myos_input --smoke strict_handles   -> 43 PASS, 0 FAIL, 0 WARN
./myos_input --smoke all              -> 1170 PASS, 0 FAIL, 0 WARN

Notes / next obvious work
-------------------------
- This closes an important gap after the sparse handle-table rewrite: pseudo
  handles are now not confused with table handles, but can be converted into
  real handles through the same public API foreign Win32 code expects.
- Next strong handle/security step: GetHandleInformation/SetHandleInformation
  with HANDLE_FLAG_INHERIT and HANDLE_FLAG_PROTECT_FROM_CLOSE, then make
  DuplicateHandle/CloseHandle honor protect-from-close.
