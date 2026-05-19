# myOS v217 - public HANDLE slot cache fastpath

v217 extends the v215/v216 ObjectId-slot model into the public per-process HANDLE table.

## Highlights

- Public handle-table entries now cache the decoded Object Manager slot alongside:
  - public HANDLE value
  - ObjectHandle/ObjectId
  - object type
  - granted access
  - inherit/protect flags
- The TLS last-handle cache now carries the object slot too.
- `mywin_resolve_handle_public_ex()` can return ObjectHandle + type + object slot + access in one table lookup.
- Duplicate/inherit hotpaths carry the source entry's cached object slot forward.
- Object ref bumps use `mywin_add_object_ref_by_slot()` for slot-coded Event/Mutex/Semaphore/Section/Timer/Token objects before falling back to legacy object lookup.
- `MyHandleInfo` diagnostics now expose `object_slot`.

## Architecture note

This keeps the Win32 public HANDLE opaque while making the internal dispatch token slot-oriented:

```
public HANDLE
  -> per-process sparse handle table slot
  -> cached { ObjectHandle, Type, ObjectSlot, Access }
  -> direct type-local object array index
```

Names remain a control-plane concern (`Name -> ObjectHandle`). Hot paths use already-open handles and dispatch by numeric slots.

## Smokes

- strict_handles v217 public HANDLE entries cache object slots
- strict_handles v217 handle-slot diagnostic lookup
- v217 public handle-slot cache benchmark

Validated with:

```
make clean && make -j$(nproc)
./myos_input --smoke strict_handles
./myos_input --smoke all
```
