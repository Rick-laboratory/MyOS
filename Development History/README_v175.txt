myOS v175 - damage-rect compositor
BUILD: myos_v175_damage_rect_compositor

This version advances the v174 lazy compositor from idle-skip to actual
damage-driven redraw:

- The render thread keeps a small coalescing damage-rect list.
- Cursor motion marks old/new cursor rectangles instead of forcing full-frame repaint.
- Non-pointer render-state changes still conservatively promote to full-frame damage.
- Framebuffer primitives now support a compositor clip rectangle.
- redraw() sets the clip per damage rect, redraws the scene through that clip, then flips only that rectangle to the physical framebuffer.
- fb_clear(), fb_rect(), fb_pixel(), font drawing, wallpaper drawing, GDI/OOP surface paths respect clipping through the framebuffer primitive layer.

This is intentionally still conservative: window moves, resize, HWND/GDI/OOP/terminal state changes currently promote to full damage for correctness.  The important architecture change is that redraw is no longer inherently fullscreen: the renderer can now consume explicit damage rectangles, and the next versions can replace conservative full promotion with per-window/child/control damage.
