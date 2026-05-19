myOS v73.3 - HWND StateProbe button feedback + explicit test procedure

Goal
----
v73.2 fixed the POSIX shm backing-name truncation bug. The HWND StateProbe now mapped
successfully, but its buttons felt like placeholders because the render path auto-mapped
again immediately and the buttons had weak visible feedback.

Changes
-------
- StateProbe starts unmapped on purpose.
- Map explicitly opens/maps Global\myos.v73.hwnd.state.section.
- Refresh no longer auto-maps. If unmapped, it stays unmapped and says so.
- CloseMap really unmaps/closes and the render path does not secretly map again.
- Added visible button diagnostics:
  - mapped=YES/NO
  - clicks
  - last action
  - last mouse client x/y
  - map count
  - close count
  - refresh count
  - paint count
  - AutoRefresh ON/OFF
- Added Auto ON/OFF button. Auto only refreshes while already mapped; it never maps by itself.
- Build/version strings updated to v73.3.

Test procedure
--------------
1. Build and start:

   make clean && make
   sudo chvt 3
   sudo ./myos_input /dev/input/event1 /dev/input/event2

2. Open the probe:

   Right click desktop -> HWND StateProbe

3. Initial expected state:

   mapped=NO
   clicks=0
   lastAction=<none yet>
   section: not mapped - click Map. Refresh will NOT auto-map anymore in v73.3.

4. Click Refresh before Map:

   Expected:
   - clicks increments by 1
   - lastAction=Refresh clicked #1
   - mapped stays NO
   - status says Refresh clicked while unmapped
   - no HWND table entries are shown

   If mapped becomes YES here, the old auto-map bug is still present.

5. Click Map:

   Expected:
   - mapped=YES
   - maps increments by 1
   - view=0x... is non-null
   - status says Map OK
   - table shows Terminal/Admin/Calc/StateProbe/etc. entries

6. Click Refresh repeatedly:

   Expected:
   - clicks increments every click
   - refresh increments every click
   - lastAction changes to Refresh clicked #N
   - mapped stays YES
   - table remains visible

7. Move another window, focus another window, or open/close Calc/Terminal:

   Then click Refresh.

   Expected:
   - section seq or per-row seq changes
   - rect/title/flags may change
   - active/focused row should change color/flags when focus changes

8. Click CloseMap:

   Expected:
   - mapped=NO
   - close count increments
   - view=(nil) or 0x0
   - table disappears
   - status says CloseMap OK

   If table immediately comes back after CloseMap, the render path is still remapping incorrectly.

9. Click Refresh after CloseMap:

   Expected:
   - mapped remains NO
   - clicks/lastAction update
   - no table entries

10. Click Map again:

   Expected:
   - mapped=YES again
   - maps increments again
   - table returns

11. Toggle Auto ON:

   Expected:
   - Auto button text changes to Auto ON
   - auto=1
   - while mapped, paint/heartbeat refreshes can update counts/seq without pressing Refresh
   - CloseMap still forces mapped=NO and Auto does not remap by itself

Pass criteria
-------------
- Every button visibly changes clicks and lastAction.
- Refresh while unmapped does not map.
- CloseMap really clears the mapping and keeps it cleared.
- Map restores the mapping and HWND rows.
- Window move/focus/open/close changes are visible after Refresh or Auto ON.
