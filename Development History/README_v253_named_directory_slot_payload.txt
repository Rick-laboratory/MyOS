myOS / Linux-Win32 v253 - Named Directory typed-slot payload fast-open
======================================================================

Focus
-----
This pass continues the v250-v252 Named Object Directory work.  v251 made the
shared directory the fast-open path, and v252 added a thread-local directory
cache.  v253 makes directory entries carry the already-decoded typed payload
slot and Object Manager generation.

Why
---
Before v253, a successful directory lookup returned the canonical object's
ObjectHandle and type.  The typed Open/Create-existing path then called the
normal typed find helper, which decoded the ObjectHandle again and re-derived
the slot.  That was already O(1), but it still repeated work on every named
open.

v253 stores this payload directly in the namespace node:

    canonical name -> object type + object handle + typed payload slot + generation

The Open/Create-existing paths for Section, Event, Mutex, Semaphore and
Waitable Timer now try the directory's payload slot first and only fall back to
legacy typed lookup if that slot is stale or invalid.

Changed behavior
----------------
- MyWinNamedDirectoryEntry now carries objectSlot/objectGeneration.
- The TLS named-directory cache carries the same payload slot/generation.
- mywin_named_directory_fast_lookup_payload() returns the slot/generation along
  with the object handle/type.
- Section/Event/Mutex/Semaphore/Timer named-open paths use direct typed-slot
  validation before falling back.
- Directory epoch/stale handling remains intact.  Remove/reuse still invalidates
  old TLS entries.

Diagnostics
-----------
MyNamedDirectoryAudit adds:

    slot_fast_hits
    slot_fast_misses

Smoke coverage
--------------
New/extended checks:

    v253 named directory typed-slot fast-open
    v253 named directory typed-slot covers sections/timers

Expected smoke evidence:

    OpenEvent/OpenMutex/OpenSemaphore benchmark: slotHit == opens, slotMiss == 0
    Section/Timer benchmark: slotHit == ops, slotMiss == 0

Validation
----------
Validated with:

    make clean && make -j2
    ./myos_input --smoke all
    ./myos_input --smoke wait_real

Both smoke runs pass with 0 failures.
