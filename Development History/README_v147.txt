BUILD: myos_v147_strict_kernel_handle_table_v1

v147 hardens the public KERNEL32 handle contract.

Goal:
Public KERNEL32-style APIs must consume process handle-table handles, not raw
Object Manager backing handles.  Raw object handles remain available only to
internal/diagnostic inspection paths such as ObjectLab/Spy-style tooling.

Main changes:
1. Strict public handle resolver
   - Added mywin_resolve_handle_public().
   - Public KERNEL32 entrypoints no longer fall back to raw object handles in
     strict mode.
   - Internal mywin_resolve_handle() remains a compatibility/diagnostic resolver.

2. Runtime strict switch
   - Added MyWinSetStrictKernelHandles(BOOL).
   - Added MyWinGetStrictKernelHandles().
   - Default is strict ON.

3. Public APIs hardened
   - MapViewOfFile
   - MyWinGetSectionBackingInfo / MyWinReleaseSectionViewHandle
   - SetEvent / ResetEvent
   - ReleaseMutex
   - ReleaseSemaphore
   - SetWaitableTimer / CancelWaitableTimer
   - WaitForSingleObject / WaitForMultipleObjects
   - DuplicateHandle
   - CloseHandle
   - process-handle access checks through mywin_has_handle_access()

4. DuplicateHandle source strictness
   - Source handles are resolved from the source process handle table.
   - Raw object handles are rejected with ERROR_INVALID_HANDLE in strict mode.
   - Legal table-handle duplication still works.

5. CloseHandle strictness
   - CloseHandle no longer destroys raw Object Manager handles in public strict
     mode.
   - Double-close and invalid-handle paths remain smoke-gated.

6. AppHost pid/handle fix exposed by strict mode
   - AppHost loader privilege is now a capability overlay on the caller pid,
     not a separate pid 55 identity.
   - This prevents hProcess/hThread from being created in pid 55 and then
     returned to the caller's pid, which strict handles correctly reject.

New smoke group:
  ./myos_input --smoke strict_handles

Smoke coverage added:
- raw event rejected by WaitForSingleObject / SetEvent / ResetEvent / CloseHandle / DuplicateHandle
- raw semaphore rejected by ReleaseSemaphore
- raw mutex rejected by ReleaseMutex
- raw timer rejected by SetWaitableTimer
- raw section rejected by MapViewOfFile
- valid table handles still work for Event/Semaphore/Mutex/Timer/Section
- duplicated table handles still work
- closed duplicated handle becomes invalid
- LastError is checked for ERROR_INVALID_HANDLE where applicable

Validation:
  make clean && make -j2
  ./myos_input --smoke strict_handles
  ./myos_input --smoke all

Result:
  Build PASS
  strict_handles PASS
  all smoke PASS

Next recommended version:
  v148_dispatcher_wait_condvar_v1
