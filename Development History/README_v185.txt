myOS v185 - Handle Table Exit Ref Audit
BUILD: myos_v185_handle_table_exit_ref_audit

- per-process HANDLE tables are audited as first-class lifetime state
- process exit sweeps all handles owned by the exited PID
- parent-owned handles to exited PROCESS/THREAD objects remain valid until CloseHandle
- debug badge now shows HT total/owner/dead/orphan/sweep counters
- apphost smoke checks inherited child handles are gone after process exit
