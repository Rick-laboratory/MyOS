myOS v218 - ObjectId/ObjectHeader slot-state-generation model

Goal
----
Continue the v215-v217 slot-dispatch work, but express the Object Manager with
WinAPI/NT-adjacent terminology instead of inventing a separate fantasy layer.
The public API remains Win32/MSDN-facing. Internal diagnostics use _OBJECT_* names
for undocumented/private structures.

Key architecture
----------------
- ObjectId/ObjectHandle is still a number, but now it carries enough identity to
  validate direct dispatch:
    tag + object type + object generation + type-local slot

- Object Manager slots now carry OBJECT_HEADER-style metadata:
    Type
    Slot
    Generation
    State
    PointerCount
    HandleCount
    GrantedAccess

- Slot states added:
    FREE
    RESERVED
    PENDING_CREATE
    LIVE
    CLOSING
    ZOMBIE

- Stale ObjectIds are invalidated by generation changes. Closing/destroying an
  object advances the slot generation, so an old numeric ObjectId cannot silently
  address a new object that later reuses the same slot.

- Public HANDLE entries continue to cache ObjectHandle/ObjectType/ObjectSlot.
  v218 adds generation/state/handle-count visibility to diagnostic HandleInfo.

New diagnostic/internal APIs
----------------------------
- _ObjectDecodeObjectId(handle, &type, &slot, &generation)
- _ObjectQueryObjectHeader(handle, &header)
- _ObjectReferenceHandle(handle)
- _ObjectDereferenceHandle(handle)

Notes on nomenclature
---------------------
The model intentionally tracks NT/WinAPI vocabulary:
- public HANDLE: process-local handle-table entry
- ObjectId/ObjectHandle: numeric object-manager ticket
- ObjectHeader: object metadata/lifetime/security/name/payload identity
- ObjectType: type discriminator for dispatch
- GrantedAccess / HandleAttributes remain properties of public handle entries
- SecurityDescriptor remains object metadata

Validation
----------
- make clean && make -j$(nproc)
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all

Expected strict_handles additions:
- v218 object header uses slot/generation/state nomenclature
- v218 ObjectId generation invalidates stale object numbers
- v218 ObjectId -> ObjectHeader direct dispatch

