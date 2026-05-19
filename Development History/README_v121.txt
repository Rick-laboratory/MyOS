myOS v121 - Access Rights / Open Object Contract

BUILD:
  myos_v121_access_rights_open_object_contract

Goal:
  First real hardening pass for named kernel object access rights.
  v120 made smoke coverage wider. v121 makes those checks meaningful for
  Events, Mutexes, Semaphores, Sections and DuplicateHandle rights.

Main changes:
  - OpenEventA/OpenMutexA/OpenSemaphoreA/OpenFileMappingA now preserve the
    requested access mask in the process handle table.
  - SetEvent/ResetEvent require EVENT_MODIFY_STATE on the actual handle.
  - ReleaseMutex requires MUTEX_MODIFY_STATE on the actual handle.
  - ReleaseSemaphore requires SEMAPHORE_MODIFY_STATE on the actual handle.
  - WaitForSingleObject/WaitForMultipleObjects now distinguish:
      invalid object handle -> ERROR_INVALID_HANDLE
      valid object but missing SYNCHRONIZE -> ERROR_ACCESS_DENIED
  - MapViewOfFile now checks the mapping handle access mask:
      FILE_MAP_READ handle can map READ
      FILE_MAP_READ handle cannot map WRITE
      FILE_MAP_WRITE handle can map WRITE
  - DuplicateHandle can no longer amplify rights unless DUPLICATE_SAME_ACCESS
    preserves the source mask.
  - SD-lite private object naming was tightened: names containing .private.
    are owner-only even if they live below Global\\.

New smoke group:
  ./myos_input --smoke access

Validation:
  make clean && make -j2
  ./myos_input --smoke all

Expected result:
  BUILD: myos_v121_access_rights_open_object_contract
  SMOKE RESULT: PASS (0 failures)

Known remaining gaps:
  - This is still SecurityDescriptor-lite, not full Windows token/SID/DACL/ACE.
  - Raw handle fallback remains for old demos and is still a strict-isolation gap.
  - WaitForMultipleObjects(WAIT_ALL) remains non-atomic; this release only hardens
    access validation and LastError behavior around waits.
  - USER32 GetWindowRect/MoveWindow WARNs remain intentionally visible.
