BUILD: myos_v158_scrollwindowex_v1

v158 USER32 ScrollWindowEx / ScrollWindow v1

Scope:
- Added Win32/MSDN-facing ScrollWindowEx and ScrollWindow declarations to sdk/include/winuser.h.
- Added SW_SCROLLCHILDREN, SW_INVALIDATE, SW_ERASE, SW_SMOOTHSCROLL and ERROR/NULLREGION/SIMPLEREGION/COMPLEXREGION constants.
- Implemented ScrollWindowEx in USER32:
  - client-coordinate prcScroll/prcClip handling
  - uncovered update rectangle computation
  - NULLREGION/SIMPLEREGION/COMPLEXREGION/ERROR return contract
  - SW_INVALIDATE dirty-region integration
  - SW_ERASE erase-pending integration through the v157 paint/erase path
  - SW_SCROLLCHILDREN moves intersecting child HWNDs by dx/dy
  - SW_SCROLLCHILDREN sends WM_MOVE even for zero-delta/intersecting children
  - invalid HWND / access-denied error paths
- Implemented ScrollWindow as legacy wrapper over ScrollWindowEx:
  - always invalidates uncovered region
  - lpRect == NULL path scrolls children
- Added private MyGdiScrollWindowContent hook:
  - translates retained window GDI commands wholly inside the effective scroll source rectangle
  - uncovered/partial areas remain covered by invalidation + WM_PAINT contract
- Export registry updated:
  - winbase.c MYWIN_EXPORT(ScrollWindow), MYWIN_EXPORT(ScrollWindowEx)
  - shellapi.c user32 import registry entries
- Added smoke group user32_scroll.

Validation:
- make clean && make -j2: OK
- ./myos_input --smoke user32_scroll: 22 PASS, 0 FAIL, 0 WARN
- ./myos_input --smoke all: 900 PASS, 0 FAIL, 0 WARN

Notes / intentional limits:
- HRGN update mutation is not implemented yet because myOS still has no real region object table.
- Pixel scroll is command-buffer-lite in v158. Fully correct retained/window-surface scrolling needs real clipping/region-backed surfaces later.
- SW_SMOOTHSCROLL flag constant exists; timed smooth-scroll animation is not implemented in v158.
