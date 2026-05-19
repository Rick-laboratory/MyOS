myOS v178 - in-process visual-sequence backing cache
BUILD: myos_v178_inprocess_visual_seq_cache

Architectural step after v177:
- v174 removed unconditional idle full-redraw.
- v175 added compositor damage rects, clipping, and partial framebuffer flips.
- v176 scoped visible deltas to windows/icons/menus instead of full-screen damage.
- v177 added retained per-window backing cache for OOP top-level HWNDs.
- v178 extends the retained-cache contract to selected in-process apps.

What changed:
- HWND dispatch now advances a per-HWND visual sequence in MyWindowState after the WndProc actually runs.
- The compositor hashes all live HWND states owned by the same app capability/process when building a top-level visual signature.
- Selected self-contained in-process apps (Calc, Editor, AccessLab, SectionLab, ClipMenuLab, PaintLab, DragLab, ControlLab, ServiceLab, DialogLab, MDILab) can now reuse their per-window backing cache when their own HWND tree has not changed.
- OOP retained cache from v177 stays intact.
- Diagnostic/global-sampling apps remain uncached for correctness for now: Spy, Pump, Deadlock, ObjectLab, WaitLab, SharedBus.

Why:
- Controls/buttons/elements whose state did not change should not be redrawn just because a damage rect passes over their window.
- In-process apps needed a safe visual-version source before being cacheable.
- This keeps the Win32 direction: WndProc/message dispatch changes the app surface; compositor composes cached surfaces until that visual sequence advances.

Smoke:
- ./myos_input --smoke all
- SMOKE RESULT: PASS (0 failures)
