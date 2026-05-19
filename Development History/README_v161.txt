myOS v161 - GetDIBits / SetDIBits / StretchDIBits-lite v1
===========================================================

BUILD: myos_v161_dibbits_transfer_v1
Base:  myos_v160_create_dibsection_v1

Validation performed:
- make clean && make -j2
- ./myos_input --smoke gdi_dibbits
- ./myos_input --smoke gdi_dibsection
- ./myos_input --smoke gdi_bitmap_dc
- ./myos_input --smoke gdi_region
- ./myos_input --smoke all

Results:
- Build: PASS, 0 warnings
- gdi_dibbits:    22 PASS, 0 FAIL, 0 WARN
- gdi_dibsection: 26 PASS, 0 FAIL, 0 WARN
- gdi_bitmap_dc:  30 PASS, 0 FAIL, 0 WARN
- gdi_region:     34 PASS, 0 FAIL, 0 WARN
- all:           982 PASS, 0 FAIL, 0 WARN

Implemented in v161:
- GDI_ERROR constant in public SDK wingdi.h.
- GetDIBits export/prototype/implementation.
- SetDIBits export/prototype/implementation.
- StretchDIBits export/prototype/implementation.
- SetDIBitsToDevice export/prototype/implementation as a narrow DIB-to-DC bridge.
- 32-bpp BI_RGB / DIB_RGB_COLORS transfer path.
- DWORD-aligned 32-bpp scanline helpers.
- top-down DIB transfer via negative biHeight.
- bottom-up DIB transfer via positive biHeight.
- GetDIBits lpvBits == NULL metadata query path.
- MSDN selected-HBITMAP guard for GetDIBits and SetDIBits.
- SetDIBits into compatible bitmaps and DIBSections when unselected.
- StretchDIBits SRCCOPY to memory DC and window/paint DC command snapshot path.
- nearest-neighbor StretchDIBits scaling for the supported SRCCOPY path.
- unsupported ROP and unsupported DIB formats fail instead of fake-supporting them.

Intentional limits / not faked:
- Only 32-bpp BI_RGB / DIB_RGB_COLORS is implemented.
- No DIB_PAL_COLORS / palette index mode.
- No 1/4/8/16/24-bpp conversion yet.
- No BI_BITFIELDS masks.
- No BI_JPEG / BI_PNG / compressed bitmap handling.
- No ICM color management.
- No HALFTONE or full stretch mode stack.
- No full ROP engine beyond SRCCOPY for these DIB paths.
- SetDIBitsToDevice is still a narrow bridge and not the complete scan-start/clipping contract.

Suggested next step:
- v162: StretchBlt / SetStretchBltMode / COLORONCOLOR foundation,
  now that DIBSection and DIB transfer paths exist.
