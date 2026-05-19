myOS / Linux-Win32
BUILD: myos_v174_lazy_compositor_idle_skip

This version removes the unconditional 60 Hz full-frame repaint from the desktop compositor loop.

Root cause:
- The main render thread always did d->dirty = 1 before redraw().
- That made the existing dirty flag meaningless and redrew background, desktop icons, windows, controls, taskbar, menus and OOP frames even when no visible state changed.

Fix:
- Desktop now keeps a render signature of visible compositor state.
- The render thread still pumps Win32 queues, ProcessHost IPC, terminal output and OOP GDI/surface state.
- It redraws only when the signature changed or an input path explicitly marked the desktop dirty.
- WM/input paths that mutate visible state keep setting d->dirty.
- OOP child updates are detected by gdi_sequence/surface_seq and related ProcessHost counters.
- Terminal output/blink/input is part of the signature.

Scope:
- This is the first lazy-compositor step: idle full-frame redraw is gone.
- It is not yet full DWM dirty-rectangle/per-window backing-store composition.
- When a frame is dirty, wm_draw() still redraws the whole frame. The next architectural step is damage rectangles and per-window backing stores.
