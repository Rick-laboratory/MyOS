# myOS v121 Access Rights / Open Object Contract

v121 is the first hardening pass after the v120 smoke expansion. It focuses on
named kernel object handle rights rather than adding new apps.

## Hardened public contract

The following APIs now respect the access mask stored in the process-local handle
table instead of relying on object existence alone:

- `SetEvent` / `ResetEvent` require `EVENT_MODIFY_STATE`.
- `ReleaseMutex` requires `MUTEX_MODIFY_STATE`.
- `ReleaseSemaphore` requires `SEMAPHORE_MODIFY_STATE`.
- `WaitForSingleObject` and `WaitForMultipleObjects` require `SYNCHRONIZE`.
- `MapViewOfFile` requires `FILE_MAP_READ` or `FILE_MAP_WRITE` according to the
  requested view access.
- `DuplicateHandle` no longer allows a caller to request a target access mask
  that is not covered by the source handle.

## Open-object behavior

`OpenEventA`, `OpenMutexA`, `OpenSemaphoreA` and `OpenFileMappingA` now form a
smoke-gated path for desired-access testing. The desired mask is stored on the
returned process-local handle and later enforced by the relevant operation.

## SecurityDescriptor-lite rule

Full Windows security descriptors are not implemented yet. v121 tightens the
existing SD-lite model so names containing `.private.` are owner-only even under
`Global\\`. This gives AccessLab and smoke tests a deterministic private-object
path until real SID/DACL/ACE support lands.

## Smoke coverage

New group:

```bash
./myos_input --smoke access
```

Covered cases include:

- wait-only event handle cannot call `SetEvent`
- modify-only event handle cannot wait
- duplicate handle cannot amplify rights
- wait-only mutex can acquire but cannot release
- wait-only semaphore can consume but cannot release
- read-only section handle cannot request a write mapping
- write-capable section handle can write through the mapping
- non-owner can open public named object for read
- non-owner cannot open `.private.` named Event/Mutex/Semaphore/Section

## Still intentionally not solved

- real token/SID/DACL/ACE parsing
- inheritable security descriptors from `SECURITY_ATTRIBUTES`
- atomically correct dispatcher `WAIT_ALL`
- removal of raw-handle fallback
