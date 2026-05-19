myOS v82.1 - WSTS dirty-signal / WINDOWPOS crash fix

Fixes v82 crash when moving certain legacy labs (AccessLab, PumpLab,
DeadlockLab, DragLab, ServiceLab).

Root cause:
  The HWND state publisher posted WM_WINDOWPOSCHANGED back to the source HWND
  with lParam containing only the WSTS state serial. After v82, legacy apps
  correctly treated WM_WINDOWPOSCHANGED.lParam as WINDOWPOS*. That synthetic
  self-signal therefore dereferenced a state serial as a pointer and crashed.

Change:
  - Real Win32/MSDN messages still go through wm_send_window_message.
  - WSTS publish now only notifies subscribers with WM_MYOS_HWND_STATE_DIRTY.
  - Source HWND no longer receives synthetic self-signals from the dirty path.

Test:
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

  Open and move/resize:
    AccessLab, PumpLab, DeadlockLab, DragLab, ServiceLab
  Also regression-test:
    Calc, Editor, Spy++, WaitLab, SectionLab, ObjectLab, StateProbe live dirty.
