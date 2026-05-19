myOS v40 - Refactor Floor Release
=================================

Goal: keep v39.2 behavior stable while removing the first real hygiene debt.

Changes:

- Added shared owner-draw control helpers in mycontrols.c/.h:
  - MYBUTTONCONTROL
  - DrawButtonControl()
  - HitTestButtonControl()
  - PtInRectXY()
  - PushLogLineA()
- ServiceLab now uses the shared button draw/hit-test path and shared log-ring
  helper. This preserves the v39.2 click fix and makes future labs less likely
  to drift between paint-coordinates and input-coordinates.
- window.c got a real app-window creation helper:
  - wm_reserve_window_slot()
  - wm_prepare_app_window()
  - wm_finish_app_window()
  This removes the bulk of wm_add_* copy-paste while keeping each public
  wm_add_* wrapper readable.
- Added semantic ID typedefs in mytypes.h:
  - MyPid
  - MyTid
  - MyCapId
  Capability.id and ipc_targets now use MyCapId. Internally this is still
  uint32_t, but the code now names the intended meaning.
- Added LOCK_ORDER.md as the first central lock hierarchy note.
- Makefile banner updated to v40.

Smoke test:

    make clean && make
    sudo ./myos_input /dev/input/event1 /dev/input/event2

Recommended test path:

1. Open ServiceLab and confirm buttons still react immediately.
2. Open/close several labs to confirm closed window slots are reused.
3. Open PaintLab/DragLab/ControlLab after closing another app; v40 now routes
   those through the same slot-reuse helper too.
4. Open ObjectLab after ServiceLab and check SERVICE objects still appear.
