# myOS / Linux-Win32 v251 - Central Named Object Directory Fast Open

v251 moves the v250 named-object namespace overlay closer to a real NT-style
Object Directory.

v250 already used the central directory to catch cross-type name collisions such
as CreateEvent("X") followed by CreateMutex("X").  The typed hash tables were
still the primary open path, though: OpenEvent/OpenMutex/OpenSemaphore and the
Create* existing-object cases usually walked the per-type name hash after the
preflight.

v251 changes that:

- The central named directory now has an O(1) free-stack with mark bits, instead
  of scanning the whole directory array for a free entry on insert.
- Named Create/Open paths for Sections, Events, Mutexes, Semaphores, and
  Waitable Timers first resolve through the shared directory:

      canonical name -> directory entry -> object type + object handle ->
      direct typed slot lookup

- The per-type name hashes remain as conservative fallback paths for legacy or
  stale-directory repair cases.
- Cross-type collisions are still rejected before typed-object creation.
- Directory removals return entries to the free stack so name reuse stays O(1).
- A new diagnostic surface, `MyNamedDirectoryAudit`, exposes entries, free slots,
  fast hits/misses, stale hits, inserts/removes, and conflict counters.
- Smoke now has a v251 named directory benchmark proving that repeated
  OpenEvent/OpenMutex/OpenSemaphore calls are served by the shared directory
  fast-open path before the typed fallback.

Relevant smoke line:

    [PASS] strict_handles v251 central named object directory fast-open

This is intentionally not the final Object Manager directory model yet.  A true
NT-like directory would use directory entries as the authoritative namespace
nodes for all named objects.  v251 is the next safe step: the directory is now
both the cross-type arbiter and the fast-open name resolver, while existing typed
object payload tables remain stable.
