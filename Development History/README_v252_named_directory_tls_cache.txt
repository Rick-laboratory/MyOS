# myOS / Linux-Win32 v252 – Named Directory TLS Fast-Open Cache

Focus: continue the v250/v251 Object Namespace work without touching the
v246-v250 WaitBlock/Dispatcher semantics.

## What changed

- Added a small per-thread direct-mapped cache for the central named object
  directory fast-open path.
- Repeated same-thread opens of stable named objects now avoid the shared
  directory mutex after the first authoritative directory lookup.
- Directory insert/remove/update bumps a global directory epoch; cached entries
  store the epoch and are discarded when the namespace changes.
- Directory stale-handle observations also bump the epoch, so a stale fast-open
  entry cannot survive into later opens.
- `MyNamedDirectoryAudit` now exposes TLS cache hit/miss/store/epoch counters.
- Smoke includes a repeated `OpenEvent/OpenMutex/OpenSemaphore` benchmark and an
  epoch-safety test that removes an Event, recreates the same name as a Mutex,
  then verifies the old Event cache cannot be used.

## Design intent

v251 made the central named directory the normal fast-open path. v252 keeps that
path authoritative but removes the global directory lock from the common
same-thread repeated-open case:

```
canonical name + wanted type
    -> TLS directory cache, epoch checked
    -> cached object handle
    -> typed slot validation as before
```

On miss or epoch mismatch, lookup falls back to the shared central directory and
refreshes the TLS entry. Typed tables remain as conservative stale/fallback
payload indexes.

