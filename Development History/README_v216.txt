myOS v216 - unified Object Manager slot handles for all core kernel-object families

What changed:
- Event/Mutex/Semaphore direct object-slot lookup from v215 is extended to:
  - Sections / FileMappings
  - Waitable Timers
  - Tokens
  - Process objects
  - Thread objects
  - Service objects / SCM service entries
- Runtime process/thread objects are now created as _OBJECT_TYPE_PROCESS / _OBJECT_TYPE_THREAD slot-coded ObjectHandles.
- CreateProcessA reserves a ProcessLite slot first and derives PROCESS/THREAD ObjectHandles from that slot.
- GetCurrentProcess/GetCurrentThread pseudo-handle materialization resolves through the ProcessLite table instead of legacy raw 0x50000000/0x51000000 bit patterns.
- OpenProcess/OpenThread resolve pid/tid to slot-coded ObjectHandles.
- mywin_process_id_from_handle now reads owner_pid from _ObjectectInfo for slot-coded PROCESS objects.
- SCM/service handles now use _OBJECT_TYPE_SERVICE slot handles instead of raw 0x5c... service values.

Why:
- Public HANDLE -> per-process handle table -> ObjectHandle -> Object Manager slot is now the common fast path.
- Name lookup remains only the name-to-object-id/index step; repeated object access becomes ObjectId -> array slot.
- This removes more hidden linear scans from the kernel-object path without changing public Win32 handle semantics.

Smoke:
- strict_handles v216 unified object-slot handles
- strict_handles v216 unified direct object-slot lookup
- v216 unified object-slot lookup benchmark
