myOS v219 - USER32 HWND header / slot-state dispatch model

Goal
----
Carry the v218 ObjectHeader lesson into USER32: HWND values are now opaque
public USER handles that decode internally to a window slot plus generation,
and each slot carries a small WindowHeader-style state record.  This makes
HWND resolution the first dispatch step instead of scattering repeated
IsWindow/linear-search checks through later USER32 paths.

Highlights
----------
- HWND values now use an encoded slot/generation form for newly created windows.
- HWNDEntry carries USER object metadata: hwnd_slot, hwnd_generation, hwnd_state.
- Added _HwndState: FREE, RESERVED, NCCREATE, LIVE, DESTROY_PENDING,
  NCDESTROY, ZOMBIE.
- hwnd_decode() decodes public HWND values into slot/generation.
- hwnd_query_header() returns an OBJECT_HEADER-style diagnostic view for HWNDs.
- find_entry_index() now uses the encoded slot/generation fast path first;
  linear search remains only as a legacy fallback.
- hwnd_get_owner_pid()/hwnd_get_owner_tid() use the same fast resolver.
- USER32-local MyWinWindowInfo lookup/allocation now aligns to the decoded HWND
  slot before falling back to legacy scanning.
- CreateWindowExA transitions the HWND state from NCCREATE to LIVE after
  WM_NCCREATE/WM_CREATE succeed.
- DestroyWindow marks DESTROY_PENDING before WM_DESTROY and invalidates the
  old HWND after teardown; slot reuse increments generation so stale HWNDs do
  not resolve to new windows.

Validation
----------
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all

New smoke coverage
------------------
- v219 HWND header uses slot/generation/state nomenclature
- v219 HWND -> WindowHeader direct dispatch
- v219 HWND generation invalidates stale window numbers

Architecture note
-----------------
This is intentionally not a full rewrite of every USER32 branch yet.  v219
puts the central HWND identity/resolution primitive in place.  Follow-up work
can move message dispatch, hit-testing, painting, focus/capture, dialog routing
and z-order transitions onto state-table/action-table paths instead of local
if/switch webs.
