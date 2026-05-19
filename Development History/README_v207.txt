myOS v207 - GetHandleInformation / SetHandleInformation / protect-from-close
=============================================================================

Goal
----
Continue the v205/v206 handle-table cleanup with the next real Win32 handle
property layer. HANDLE_FLAG_INHERIT and HANDLE_FLAG_PROTECT_FROM_CLOSE are
per-handle table attributes, not Object Manager attributes, so they belong in
the sparse per-PID handle entry created in v205.

What changed
------------
- Added HANDLE_FLAG_INHERIT and HANDLE_FLAG_PROTECT_FROM_CLOSE to the public SDK.
- Added public KERNEL32 exports:
  - GetHandleInformation
  - SetHandleInformation
- MyWinHandleEntry now stores protect_from_close separately from the object and
  from the inherit flag.
- GetHandleInformation reports the current process-local handle flags.
- SetHandleInformation mutates only the selected dwMask bits, matching Win32's
  per-handle property model.
- Diagnostic MyGetHandleInfo now exposes both flags:
  - MYWIN_HANDLE_FLAG_INHERIT
  - MYWIN_HANDLE_FLAG_PROTECT_FROM_CLOSE
- CloseHandle now honors HANDLE_FLAG_PROTECT_FROM_CLOSE and fails without
  destroying the table entry or dropping the Object Manager reference.
- DuplicateHandle(DUPLICATE_CLOSE_SOURCE) now preflights protect-from-close and
  fails before allocating a target handle when the source handle is protected.
- DuplicateHandle bInheritHandle remains the authority for the new handle's
  HANDLE_FLAG_INHERIT bit. The duplicate handle does not blindly copy the source
  handle's inherit/protect flags.
- Pseudo handles remain not-table entries: GetHandleInformation on a pseudo
  handle fails with ERROR_INVALID_HANDLE.

Why this matters
----------------
v205 made the table scalable and process-local. v206 made pseudo handles
materializable. v207 completes the first public handle-property layer foreign
Win32 code expects before CreateProcess inheritance, DuplicateHandle and close
semantics become truly reliable.

Smoke updates
-------------
strict_handles now verifies:
- fresh handles report no flags
- HANDLE_FLAG_INHERIT can be set and cleared
- diagnostic handle enumeration mirrors the inherit bit
- DuplicateHandle(..., bInheritHandle=FALSE) clears inherit on the new handle
- DuplicateHandle(..., bInheritHandle=TRUE) sets inherit on the new handle
- HANDLE_FLAG_PROTECT_FROM_CLOSE can be set and cleared
- CloseHandle(protected) fails and leaves the handle live
- DUPLICATE_CLOSE_SOURCE honors protect-from-close before target allocation
- pseudo handles are rejected by GetHandleInformation

Validated
---------
make clean && make -j$(nproc)
./myos_input --smoke strict_handles   -> 65 PASS, 0 FAIL, 0 WARN
./myos_input --smoke all              -> 1192 PASS, 0 FAIL, 0 WARN

Notes / next obvious work
-------------------------
- Next strong process/handle step: wire STARTUPINFOA standard handles
  (STARTF_USESTDHANDLES, GetStdHandle/SetStdHandle) onto the handle-table model.
  That will make console/apphost inheritance much closer to real Win32 before
  PE loader work starts depending on stdin/stdout/stderr behavior.
