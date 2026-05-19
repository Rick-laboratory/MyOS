myOS v26/v27 - Named Events + WaitLab + DuplicateHandle Lite
=============================================================

Built on v25 Object Manager Lite.

v26 adds the first real Win32/NT-style sync primitive path:

  CreateEventA
  OpenEventA
  SetEvent
  ResetEvent
  WaitForSingleObject
  WaitForMultipleObjects

New app:

  Start menu -> WaitLab
  Hotkey: F13 if your keyboard/device exposes KEY_F13

WaitLab buttons:

  Create Event   - creates a fresh unique Local\myos.waitlab.manual.N manual-reset EVENT each click
  Open Event     - opens the latest created named event and keeps the opened ref so ObjectLab REF rises
  Set            - signaled=yes
  Reset          - signaled=no
  Wait 100       - WaitForSingleObject(handle, 100)
  Wait 1000      - WaitForSingleObject(handle, 1000)
  Create 3       - creates a fresh unique trio each click, event[1] starts signaled
  WaitAny        - WaitForMultipleObjects(3, ..., FALSE, 100)
  WaitAll        - WaitForMultipleObjects(3, ..., TRUE, 100)
  Duplicate      - DuplicateHandle Lite; keeps duplicated refs so ObjectLab REF rises visibly
  Close All      - CloseHandle on created/opened/duplicated/multi handles

v27-lite adds:

  OpenFileMappingA
  DuplicateHandle

Important architectural note:

This is not yet a full NT per-process handle-table implementation. It is the
bridge step: object refcounts, named lookup, duplicated/opened references and
waitable objects are now visible. The next clean step would be replacing the
current direct HANDLE value with:

  process-local handle slot -> global object pointer + granted access

ObjectLab was updated too:

  It now shows EVENT rows.
  It shows object flags.
  For EVENT flags:
    bit 0 = signaled
    bit 1 = manual-reset

Best test flow:

  1. Open ObjectLab.
  2. Open WaitLab.
  3. In WaitLab click Create Event multiple times.
  4. In ObjectLab click Refresh; multiple unique EVENT rows should appear.
  5. Click Set/Reset in WaitLab and Refresh ObjectLab; flags should change.
  6. Click Open Event or Duplicate repeatedly; the current EVENT REF should rise visibly.
  7. Click Close All; EVENT objects should vanish after refs are released.
  8. Click Create 3 -> WaitAny should return WAIT_OBJECT_0+1.
  9. WaitAll should timeout until all three events are signaled.

Build:

  make clean && make
  ./myos_input

Build badge should read:

  v27 EVENTS+WAITLAB


v27.1 WaitLab visibility fix
----------------------------

Changes after first test screenshot:

  Create Event now uses unique names:
    Local\myos.waitlab.manual.1
    Local\myos.waitlab.manual.2
    ...

  So repeated clicks create repeated EVENT rows instead of reopening the same
  hardcoded object. The newest event becomes CURRENT for Set/Reset/WaitOne.

  Create 3 also uses unique trio names each click:
    Local\myos.waitlab.multi.N.0/.1/.2

  Open Event now opens the latest created event and keeps the opened handle ref.
  DuplicateHandle now keeps every duplicated ref up to a small lab limit.
  Because v27-lite still uses direct object handles, duplicate handles have the
  same numeric HANDLE for now, but REF in ObjectLab and the WaitLab status line
  now make the effect visible.
