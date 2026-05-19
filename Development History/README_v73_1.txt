myOS v73.1 - HWND StateProbe visibility + mmap fix

Fixes after live test:
- HWND StateProbe now clears/fills its OOP GDI client area, so it no longer appears transparent over desktop icons.
- Child MapViewOfFile now opens read-only mappings with shm_open(..., O_RDONLY) and only uses O_RDWR when FILE_MAP_WRITE is requested.
- MapViewOfFile failure status now reports kernel_ok, kernel_error, returned shm name and map byte count.
- v73 WindowState section name/layout stay compatible: Global\myos.v73.hwnd.state.section.

Test:
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2
  Rechtsklick -> HWND StateProbe
  Click Map / Refresh.

Expected:
- probe client background is solid/dark, no icon bleed-through.
- Map succeeds and entries show HWND/PID/TID/rect/flags/seq/title.
