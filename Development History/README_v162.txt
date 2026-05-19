myOS v162 - StretchBlt / StretchMode foundation v1
===================================================

BUILD: myos_v162_stretchblt_mode_v1
Base:  myos_v161_dibbits_transfer_v1

Validated:
  make clean && make -j2
  ./myos_input --smoke gdi_stretchblt
  ./myos_input --smoke gdi_dibbits
  ./myos_input --smoke gdi_dibsection
  ./myos_input --smoke gdi_bitmap_dc
  ./myos_input --smoke gdi_region
  ./myos_input --smoke all

Implemented in v162:
  - StretchBlt export and SDK prototype
  - GetStretchBltMode / SetStretchBltMode
  - BLACKONWHITE / WHITEONBLACK / COLORONCOLOR / HALFTONE
  - STRETCH_ANDSCANS / STRETCH_ORSCANS / STRETCH_DELETESCANS / STRETCH_HALFTONE aliases
  - Per-DC stretchMode state, default BLACKONWHITE
  - SRCCOPY StretchBlt memory-DC source -> memory-DC destination
  - SRCCOPY StretchBlt memory-DC source -> window/paint snapshot command
  - COLORONCOLOR-style nearest sampling for expand/shrink
  - Negative width/height mirror semantics when source/destination signs differ
  - Destination clip-region respect for memory DC targets
  - Invalid StretchBlt ROP fails instead of pretending compliance

Intentional limits / not faked:
  - HALFTONE is accepted as DC state but not yet a high-quality resampling filter.
  - StretchBlt currently supports SRCCOPY only.
  - Source DC must be a memory DC with selected HBITMAP.
  - No Pattern/Destination ROP stack, no SetBrushOrgEx/HALFTONE origin contract.
  - No AlphaBlend / TransparentBlt / color management / driver capability model yet.

Next likely step:
  v163 - PatBlt / ROP brush foundation, or AlphaBlend / TransparentBlt-lite.
