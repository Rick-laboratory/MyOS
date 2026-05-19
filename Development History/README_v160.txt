myOS v160 - CreateDIBSection / DIB Bitmap Memory Bridge v1
=================================================================

Goal
----
Add the first exact, deliberately bounded CreateDIBSection path so Win32-style
code can create a directly writable DIB-backed HBITMAP, select it into a memory
DC, write pixels through ppvBits, and observe the same storage through GDI calls.

Build tag
---------
BUILD: myos_v160_create_dibsection_v1

Main changes
------------
- Added SDK structures/constants:
  - RGBQUAD
  - BITMAPINFOHEADER
  - BITMAPINFO
  - DIBSECTION
  - BI_RGB
  - BI_BITFIELDS
  - DIB_RGB_COLORS
  - DIB_PAL_COLORS
- Added CreateDIBSection export.
- Implemented v160 CreateDIBSection support for the first important path:
  - hSection == NULL
  - dwOffset == 0
  - iUsage == DIB_RGB_COLORS
  - BITMAPINFOHEADER
  - BI_RGB
  - 32 bpp
  - top-down DIBs via negative biHeight
  - bottom-up DIBs via positive biHeight
  - ppvBits returns the writable bitmap storage pointer
- Extended HBITMAP metadata with DIBSection state, original BITMAPINFOHEADER,
  DIB orientation, and DIB bit storage pointer.
- Updated GetObjectA(HBITMAP):
  - sizeof(BITMAP) returns BITMAP metadata
  - sizeof(DIBSECTION) returns DIBSECTION for CreateDIBSection bitmaps
  - dsBm.bmBits points at the same ppvBits storage
  - dsBmih preserves signed biHeight
- Updated Memory DC pixel access to respect top-down and bottom-up DIBSection
  row mapping.
- SetPixel/GetPixel, FillRect and BitBlt now operate correctly on selected
  DIBSection bitmaps.
- DeleteObject(HBITMAP) protection still blocks deletion while selected.
- Export registry / Shell known-export list extended for CreateDIBSection.
- Smoke coverage added: ./myos_input --smoke gdi_dibsection

Validation
----------
    make clean && make -j2
    ./myos_input --smoke gdi_dibsection
    ./myos_input --smoke gdi_bitmap_dc
    ./myos_input --smoke gdi_region
    ./myos_input --smoke all

Observed result from this package:
    BUILD: PASS
    BUILD WARNINGS: 0
    gdi_dibsection: 26 PASS, 0 FAIL, 0 WARN
    gdi_bitmap_dc: 30 PASS, 0 FAIL, 0 WARN
    gdi_region: 34 PASS, 0 FAIL, 0 WARN
    all: 960 PASS, 0 FAIL, 0 WARN

Known limitations intentionally left for later
----------------------------------------------
- hSection-backed DIBSections are rejected; real section/file-mapping backed
  DIB storage belongs with the future Section/GDI bridge.
- Only BI_RGB 32-bpp DIB_RGB_COLORS is implemented in v160.
- DIB_PAL_COLORS, palettes, <= 24 bpp formats, BI_BITFIELDS masks,
  BITMAPV4HEADER/BITMAPV5HEADER, color management and compression formats are
  not fake-supported.
- No GetDIBits / SetDIBits / StretchDIBits yet.
- No StretchBlt / alpha / color conversion / full ROP engine yet.

Next natural step
-----------------
v161 should build GetDIBits / SetDIBits / StretchDIBits-lite, now that
CreateDIBSection gives apps a real writable bitmap-memory bridge.
