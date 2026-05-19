myOS v120 - Smoke Coverage Tripwires
====================================

Purpose
-------
v120 is a safety-net release. It does not try to stabilize every old app/lab.
It broadens the automated smoke gate so future MSDN-contract refactors trip
immediately when they break core surfaces that were previously only checked by
manual Lab sessions.

Build tag
---------
BUILD: myos_v120_smoke_coverage_tripwires

What changed
------------
- Expanded ./myos_input --smoke all with new groups:
  - gdi
  - menu
  - capture
  - ipc_section
  - handle_invalid
  - wait_invalid
  - last_error
- Kept existing groups:
  - kernel32
  - user32
  - comdlg
  - services
  - apphost
- Converted the old invalid-handle WARNs into hard PASS/FAIL checks:
  - CloseHandle(NULL)
  - CloseHandle(INVALID_HANDLE_VALUE)
  - double CloseHandle
  - WaitForSingleObject(NULL)
  - WaitForSingleObject(INVALID_HANDLE_VALUE)
  - WaitForMultipleObjects invalid parameter / invalid handle paths
- Added LastError discipline for the new smoke-gated invalid paths.

Important remaining known gap
-----------------------------
The user32 smoke still emits WARNs for standalone CreateWindowExA windows that
are not backed by a WindowManager compositor slot:
- GetWindowRect
- MoveWindow / SetWindowPos

These are intentionally left as WARN in v120 because the next proper fix belongs
in the USER32/window manager contract pass, not in this smoke-coverage pass.

Validation
----------
make clean && make -j2
./myos_input --smoke all

Expected result:
SMOKE RESULT: PASS (0 failures)
