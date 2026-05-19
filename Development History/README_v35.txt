myOS v35 - GDI Objects + Dirty Regions

Neu:
- GDI Brush/DC objects are now visible in ObjectLab as BRUSH and transient DC objects.
- CreateSolidBrush registers BRUSH objects in the Object Manager.
- BeginPaint/GetDC register transient DC objects; EndPaint/ReleaseDC unregister them.
- SelectObject tracks brush selected-count and sets _OBJECT_FLAG_GDI_SELECTED.
- DeleteObject refuses selected brushes.
- InvalidateRect now coalesces while a WM_PAINT is already pending.
- PaintLab shows dirty/pending, dirty rect, posted paints, coalesced invalidates.

PaintLab buttons:
- Invalidate: mark dirty and queue/coalesce WM_PAINT.
- Draw Text / Draw Rect: toggle paint content.
- GetDC Draw: immediate command-buffer path.
- Clear: reset toggles and invalidate.
- Stress: 6 invalidates in one WM_COMMAND path; should coalesce.
- Validate: clears dirty/pending without painting.
- Brush+: creates an extra BRUSH object visible in ObjectLab.
- DelBrush: deletes the extra BRUSH object.
