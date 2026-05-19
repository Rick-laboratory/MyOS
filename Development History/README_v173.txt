myOS v173.4 - DialogLab owner per-HWND render geometry

BUILD: myos_v173_4_oop_nc_resize_frame_fix

Fixes the DialogLab modal-OK transparency regression without adding a DialogLab-specific compositor hack.

Root cause verified in v172 code:
- The OOP child had multiple top-level HWNDs in one process.
- CreateWindowExA for the modal dialog overwrote the process-global shared gui_w/gui_h request fields.
- child_dialoglab_render() later repainted the owner HWND using those process-global fields.
- Therefore, after OK/DestroyWindow, the owner stream could publish only a modal-sized fill area; the rest of the owner client looked transparent until a later repaint healed it.

Architecture fix:
- OOP retained-GDI rendering now resolves client dimensions per HWND through the global HWND state section when available.
- DialogLab owner and dialog probe renders keep stable fallback window sizes, so paint never depends on the last process-global CreateWindow request.
- Modal close may repaint the owner immediately because the DialogLab body intentionally exposes lastResult/status; that repaint is safe because it uses the owner HWND geometry, not the process-global CreateWindowExA request size.
- Retained per-HWND GDI streams remain window-lifetime-bound.
