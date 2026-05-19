myOS v181 - object lifetime debug fix
BUILD: myos_v181_object_lifetime_debug_fix

Focus:
- Debug OBJ counter must not behave like a monotonic app-start attempt counter.
- PROCESS/THREAD Object Manager entries now have explicit live-process lifetime refs.
- On process exit, the live lifetime refs are released exactly once; remaining external handles keep objects alive until CloseHandle(), matching Win32 object lifetime.
- AppHost loader hProcess/hThread handles are closed in the process context that actually owns those strict handle-table entries, not a hardcoded PID.
- Failed AppHost launches now terminate via the returned hProcess before closing loader-owned handles, so failed starts roll back instead of leaking PROCESS/THREAD objects.
- Debug badge now shows PROC live/exited counts in addition to OBJ/SEC.

Smoke:
./myos_input --smoke all
SMOKE RESULT: PASS (0 failures)
