myOS / Linux-Win32 v239 performance/O(1)/cache-quality pass
===========================================================

This pass focuses on hot lookup/dispatch paths without changing the public
Win32-lite API surface.  It keeps the existing smoke contract intact while
moving older fixed-table paths closer to the slot/hash-driven style used by
NT/Win32 hot paths.

Changed areas
-------------

KERNEL32 / handle tables
- PID -> process handle table lookup now uses a PID hash table instead of
  walking only the global process-table list.
- Per-process handle allocation now reuses closed slots through a free-stack
  before falling back to the legacy slow repair scan.
- Empty handle tables unlink from both the enumeration list and the PID hash.

KERNEL32 / object slots and named kernel objects
- Object slot stride was widened from 64 to 96 slots/type, keeping direct
  slot-coded lookup while fitting the current larger GDI/service tables inside
  the existing public slot field.
- Sections and waitable timers now have name-hash indexes, matching the faster
  event/mutex/semaphore lookup style.
- Waitable timer scheduling has a next-due cache so the common timeout wait path
  does not rescan the full timer table while the cached timer remains in the
  future.

ProcessHost
- myOS PID -> ProcessHost entry lookup now uses a PID hash table.
- Reclaimed ProcessHost slots are removed from the hash before reuse.

USER32 / classes and lightweight handles
- Window-class lookup now has both atom-hash and exact-name hash indexes.
- USER32-lite global memory, HMENU and HACCEL handles now have handle-hash
  indexes instead of relying primarily on table scans.
- Clipboard cleanup, menu destruction and accelerator destruction remove stale
  handle-hash entries before clearing slots.

GDI
- HBRUSH/HBITMAP/HRGN/HDC and per-HWND GDI window-state lookup now use hash
  indexes in addition to existing slot decoding and fallback scans.
- DeleteObject and selection paths resolve through the hashed find helpers.
- GDI object destruction removes hash entries before unregistering/freeing.

SCM-lite
- Service capacity was raised from 16 to 64.
- Service allocation uses a free stack instead of scanning for unused slots.
- Service name lookup is hash-indexed case-insensitively while preserving the
  slot-coded service-handle path.

Validation
----------
- Clean build: make clean && make -j2
- Smoke: ./myos_input --smoke all

Notes
-----
This pass raises the main lookup/dispatch paths substantially, but it is not a
full NT object-manager rewrite.  The intentionally larger follow-up work is a
hot/cold split of large USER32/GDI/window structs, a central object-directory
namespace, and per-object wait lists instead of broad dispatcher wakeups.
