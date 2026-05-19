myOS v163 - PatBlt / Brush ROP Foundation v1

Base
----
Built on myos_v162_stretchblt_mode_v1.

Build banner
------------
BUILD: myos_v163_patblt_rop_v1

Goal
----
Add the first exact, deliberately bounded Pattern Block Transfer path so GDI
has a real brush/destination raster-operation foundation instead of treating
all rectangle output as simple FillRect/PATCOPY-style solid fills.

MSDN contract points implemented
--------------------------------
PatBlt paints a destination rectangle using the brush currently selected in the
specified device context. The brush/pattern bits and destination bits are
combined by the documented PatBlt ROP subset:

- PATCOPY   : destination = pattern
- PATINVERT : destination = destination XOR pattern
- DSTINVERT : destination = NOT destination
- BLACKNESS : destination = black
- WHITENESS : destination = white

Implementation details
----------------------
- Added SDK ROP constants:
  PATCOPY, PATINVERT, DSTINVERT, BLACKNESS, WHITENESS.
- Added PatBlt prototype/export/known-export registry entry.
- Added BOOL PatBlt(HDC,int,int,int,int,DWORD).
- Memory DC targets apply ROPs directly to selected HBITMAP pixels.
- Destination clip region is honored for memory DC targets.
- Window/paint DC targets record a MYGDI_CMD_PATBLT command.
- MyGdiBlitWindow replays MYGDI_CMD_PATBLT against the framebuffer backbuffer,
  so DSTINVERT/PATINVERT operate on destination pixels at render time.
- Pattern-dependent ROPs currently require an explicitly selected HBRUSH because
  stock brush objects are not yet modeled.
- Destination-only ROPs (DSTINVERT/BLACKNESS/WHITENESS) do not require a brush.

Smoke
-----
New smoke group:

    ./myos_input --smoke gdi_patblt

Coverage:
- PATCOPY to memory DC
- PATINVERT XOR semantics
- DSTINVERT semantics
- BLACKNESS / WHITENESS
- SelectClipRgn clipping on memory DC target
- PATCOPY failure without selected brush in the current stock-object-lite model
- DSTINVERT without brush succeeds
- Window DC command path
- invalid ROP / invalid HDC / zero width failure paths

Validation on this build
------------------------
- make clean && make -j2: PASS
- ./myos_input --smoke gdi_patblt: 24 PASS, 0 FAIL, 0 WARN
- ./myos_input --smoke gdi_stretchblt: 17 PASS, 0 FAIL, 0 WARN
- ./myos_input --smoke gdi_dibbits: 22 PASS, 0 FAIL, 0 WARN
- ./myos_input --smoke gdi_bitmap_dc: 30 PASS, 0 FAIL, 0 WARN
- ./myos_input --smoke gdi_region: 34 PASS, 0 FAIL, 0 WARN
- ./myos_input --smoke all: see SMOKE_v163_all.log

Intentional gaps / not faked
----------------------------
- No full ternary ROP stack yet.
- No stock brush objects yet, therefore PATCOPY/PATINVERT need an explicitly
  selected HBRUSH in v163.
- No patterned/hatch brushes yet; CreateSolidBrush remains the current pattern
  source.
- No AlphaBlend/TransparentBlt yet.
- No SetBrushOrgEx / brush-origin handling yet.
- Window-visible-region/sibling clipping is still not a complete GDI driver
  model.

Next likely step
----------------
v164 should build AlphaBlend / BLENDFUNCTION / AC_SRC_ALPHA-lite, now that the
brush/destination ROP foundation exists.
