BUILD: myos_v236_winuser_geometry_surface

v236 switches from more selector micro-optimization to WINUSER API surface work.

Implemented/extended USER32 geometry + hit-test surface:
- GetClientRect
- ClientToScreen
- MapWindowPoints
- WindowFromPoint
- ChildWindowFromPointEx
- RealChildWindowFromPoint

Architecture notes:
- Geometry APIs resolve HWNDs through the existing _HWND_ACTION_GEOMETRY state path.
- Hit-test APIs resolve through _HWND_ACTION_HITTEST.
- ClientToScreen/ScreenToClient/MapWindowPoints share the same internal client-origin resolver.
- ChildWindowFromPoint/ChildWindowFromPointEx/RealChildWindowFromPoint use one common child-scan path with CWP flag masks.
- New SDK surface exports these APIs through winuser.h and the runtime export resolver.

Validation:
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all

Result:
- user32: 76 PASS, 0 FAIL, 0 WARN
- strict_handles: 85 PASS, 0 FAIL, 0 WARN
- all: 1264 PASS, 0 FAIL, 0 WARN
