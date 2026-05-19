myOS v214 - per-type object locks + named Event/Mutex/Semaphore hash lookup

Goal
----
Move the next hot path after v213 away from broad/global object-table behavior:
Event, Mutex and Semaphore named-object lookup/create/open/lifecycle now use
per-type table locks and per-type hash buckets. Refcount increments stay atomic
and lockless on the duplicate/open hot path; the type lock is only for table
lifecycle and hash-list consistency.

Implemented
-----------
- Added g_EventTableLock, g_MutexTableLock and g_SemaphoreTableLock.
- Added FNV-style named-object hash buckets for Event/Mutex/Semaphore names.
- CreateEventA/OpenEventA use canonical-name hash lookup instead of linear
  scans over g_Events for named objects.
- CreateMutexA/OpenMutexA use canonical-name hash lookup instead of linear
  scans over g_Mutexes for named objects.
- CreateSemaphoreA/OpenSemaphoreA use canonical-name hash lookup instead of
  linear scans over g_Semaphores for named objects.
- Object-type refcount increments remain atomic and do not take the type table
  lock; final lifecycle destruction removes the object from the hash while
  holding the corresponding type lock.
- Added strict_handles v214 named-object open benchmark.

Validation
----------
make clean && make -j$(nproc)
./myos_input --smoke strict_handles
./myos_input --smoke all

Observed in container
---------------------
strict_handles: 76 PASS, 0 FAIL, 0 WARN
all: PASS, 0 FAIL

v214 named object open benchmark example:
iterations=2048 opens=6144 wall_ms≈3.6-3.8 ops_s≈1.6-1.7M

Notes
-----
This version intentionally does not make a fake global lock-free object table.
The architecture is closer to the NT-style split: object identity/lifecycle is
serialized only at the owning type table, while duplicated HANDLE ref bumps stay
atomic and cheap.
