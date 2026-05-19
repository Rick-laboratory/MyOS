myOS v40.1 - WinAPI-style refactor cleanup

This is v40 with the helper naming corrected toward the project rule:
public/helper surfaces should read like WinAPI/MSDN-style C, not generic app_* utility code.

Changes from v40:
- app_util.c/.h renamed to mycontrols.c/.h.
- app_draw_button -> DrawButtonControl
- app_hit_button -> HitTestButtonControl
- app_point_in_rect -> PtInRectXY
- app_point_in_rect_slop -> PtInRectSlopXY
- app_log_push -> PushLogLineA
- app_draw_clip_text -> DrawClipTextA
- AppButton -> MYBUTTONCONTROL

The ServiceLab click fix is preserved: draw origin and hit-test origin still use the same client inset.

Build:
    make clean && make

Run:
    sudo ./myos_input /dev/input/event1 /dev/input/event2
