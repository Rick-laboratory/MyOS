myOS v215 - Object-slot handles and direct ObjectId lookup
===========================================================

Goal
----
v214 moved named Event/Mutex/Semaphore opens away from linear name scans by adding
per-type hash buckets. v215 fixes the other side of the path: once a named object
has been found, the object itself should live in an assigned slot and resolve by
index instead of repeatedly walking type/object tables.

What changed
------------
- Added Object Manager slot-handle helpers:
  - _ObjectMakeSlotHandle(type, slot)
  - _ObjectDecodeSlotHandle(handle, &type, &slot)
- _OBJECT_MAX_OBJECTS raised from 256 to 2048 so slot-coded object IDs can reserve
  type-local ranges without colliding with legacy object handles.
- Added slot-handle encoding:
  - 0x4f000000 tag
  - type in bits 23..16
  - 1-based slot in bits 15..0
- _ObjectRegister() now places slot-coded object handles into their deterministic
  Object Manager slot instead of linearly choosing any empty slot.
- _ObjectGetInfo()/SetInfo()/Security/AddRef/Release paths get direct-slot lookup
  for slot-coded objects and keep the old linear fallback for legacy/raw object
  handles.
- Event/Mutex/Semaphore object handles are now slot-coded:
  - Event slot i:     _ObjectMakeSlotHandle(_OBJECT_TYPE_EVENT, i)
  - Mutex slot i:     _ObjectMakeSlotHandle(_OBJECT_TYPE_MUTEX, i)
  - Semaphore slot i: _ObjectMakeSlotHandle(_OBJECT_TYPE_SEMAPHORE, i)
- mywin_find_event()/mywin_find_mutex()/mywin_find_semaphore() now decode the
  type-local object slot and index the backing array directly. Linear scan remains
  only as a compatibility fallback for any old-style object handles.

Architecture
------------
The target model is now:

    public HANDLE -> per-process handle table -> object handle/ObjectId
    object handle/ObjectId -> direct object slot array lookup
    name -> namespace/name index -> object handle/ObjectId

So the object storage itself is index-based. Hashing remains only the name-to-id
accelerator for string names; the object is not owned by the hash bucket.

Smoke
-----
- strict_handles now verifies that Event/Mutex/Semaphore backing object handles
  decode to type-local slots.
- Added v215 object-slot lookup benchmark over _ObjectGetInfo() using the backing
  object handles.

Expected result
---------------
- make clean && make -j$(nproc)
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all

Both pass in this build.
