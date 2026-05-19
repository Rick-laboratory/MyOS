myOS v159 - GDI Region / HRGN / Clipping Foundation v1
======================================================

Goal
----
Close the deliberate v158 ScrollWindowEx HRGN gap by adding a real GDI region
object table and wiring regions into update-region and clipping paths.

Build tag
---------
BUILD: myos_v159_gdi_region_hrgn_clip_v1

Main changes
------------
- Added HRGN as a real GDI object type:
  - CreateRectRgn
  - CreateRectRgnIndirect
  - SetRectRgn
  - OffsetRgn
  - GetRgnBox
  - CombineRgn
  - EqualRgn
  - PtInRegion
  - RectInRegion
  - DeleteObject(HRGN)
- Added region combine modes/constants:
  - NULLREGION / SIMPLEREGION / COMPLEXREGION / ERROR
  - RGN_AND / RGN_OR / RGN_XOR / RGN_DIFF / RGN_COPY
- Added USER32 update-region API surface:
  - InvalidateRgn
  - ValidateRgn
  - GetUpdateRgn
- Upgraded window update state from dirty-rect-only to a rectilinear update region.
- ValidateRect / ValidateRgn can now subtract from the current update region.
- RedrawWindow now accepts hrgnUpdate instead of ignoring it.
- ScrollWindowEx now writes hrgnUpdate with the exposed scroll region.
- Added DC clipping foundation:
  - SelectClipRgn
  - ExcludeClipRect
  - IntersectClipRect
  - GetClipBox
- FillRect on memory DCs now honors the selected clipping region.
- Export registry / Shell known-export list extended for the new functions.
- Smoke coverage added: ./myos_input --smoke gdi_region

Validation
----------
    make clean && make -j2
    ./myos_input --smoke gdi_region
    ./myos_input --smoke user32_scroll
    ./myos_input --smoke all

Observed result from this package:
    BUILD: PASS
    BUILD WARNINGS: 0
    gdi_region: 34 PASS, 0 FAIL, 0 WARN
    user32_scroll: 22 PASS, 0 FAIL, 0 WARN
    all: 934 PASS, 0 FAIL, 0 WARN
    SMOKE RESULT: PASS (0 failures)

Known limitations intentionally left for later
----------------------------------------------
- Region model is rectilinear, not a full Windows GDI path/metafile-backed region pipeline.
- Drawing clip enforcement currently covers FillRect on memory DCs; broader primitive clipping
  still needs expansion.
- No ExtCreateRegion / ExtSelectClipRgn yet.
- No window visible-region / sibling clipping / DC origin transform pipeline yet.
- ScrollWindowEx still uses command-buffer movement for existing pixels; retained surfaces and
  full compositor clipping remain future work.

Next natural step
-----------------
v160 should build CreateDIBSection / DIB memory bridge, now that HRGN/update/clip
foundation exists for paint and scroll contracts.
