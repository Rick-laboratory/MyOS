myOS v237 - WinUser export/find surface
======================================

Goal
----
Continue the post-selector USER32 surface pass.  v236 added geometry/hit-test
APIs, but several already-implemented WinUser entrypoints were still missing
from the virtual loader/export resolver.  v237 makes those APIs visible to
apphost/loader style code and adds the next practical window-search/Z-order
surface without changing the selector architecture.

Changes
-------
- Completed virtual export coverage for implemented WinUser geometry/window APIs:
  GetWindowRect, ScreenToClient, SetWindowPos, MoveWindow, GetParent, SetParent,
  GetAncestor, GetWindow, IsChild, EnumChildWindows, ChildWindowFromPoint,
  FindWindowA, EnumWindows, SetForegroundWindow, class/window long ptr APIs, etc.
- Added FindWindowExA:
  parent/child-after/class/title selector surface backed by USER32-local HWND
  metadata instead of the old title-only WindowManager lookup.
- Reworked FindWindowA to route through FindWindowExA(NULL,NULL,...), so class
  and title matching use the same path.
- Added GA_ROOTOWNER support to GetAncestor.
- Added GetTopWindow and BringWindowToTop on top of USER32-local Z-order state.
- Added SDK prototypes/macros and loader exports for the new surface.

Validation
----------
New user32 smokes:
- v237 WinUser geometry/window APIs are virtual exports
- v237 FindWindowExA honors parent/after/class/title selectors
- v237 GetAncestor supports root-owner surface
- v237 GetTopWindow/BringWindowToTop use local Z-order

Validation commands:
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all

Result:
- Build: 0 warnings
- user32: 80 PASS, 0 FAIL, 0 WARN
- strict_handles: 85 PASS, 0 FAIL, 0 WARN
- all: SMOKE RESULT: PASS
