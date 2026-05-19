myOS v73.2 - HWND StateProbe POSIX shm backing-name fix

Fixes after live test:
- v73.1 made the StateProbe visible, but MapViewOfFile still failed in the OOP child with shm_open errno=2.
- Root cause: generated POSIX shm backing name could be exactly 64 bytes, while kernel_map_name is 64 bytes including NUL. The name was truncated during IPC ACK, so the child opened a different shm object name than the parent created.
- Section backing names are now compact: /myos_sec_<handle>_<fnvhash>.
- StateProbe shows click/action counters so button delivery is visibly testable.

Test:
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2
  Rechtsklick -> HWND StateProbe

Expected:
- StateProbe maps Global\myos.v73.hwnd.state.section successfully.
- hMap/view are non-zero.
- Entries for Terminal/Calc/StateProbe/etc. appear after Refresh or automatically.
