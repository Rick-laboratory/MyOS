myOS v177 - per-HWND backing cache for OOP windows
BUILD: myos_v177_per_hwnd_backing_cache

Goal
- v174 stopped unconditional idle redraw.
- v175 introduced damage rects, framebuffer clipping, and partial flips.
- v176 scoped visible deltas to old/new window, icon, taskbar and menu rectangles.
- v177 adds a DWM-style retained backing cache for OOP top-level HWNDs.

What changed
- OOP top-level windows now render into a per-window cached backing bitmap when their visual signature changes.
- If the OOP child GDI/surface sequence, HWND, process id, size, titlebar/focus state and status are unchanged, the compositor reuses the cached pixels.
- Damage passes blit only the clipped portion of that backing cache into the desktop backbuffer.
- Legacy in-process apps remain on the direct blit path for now because their internal visual state is not yet fully represented in the Window record.

Why it matters
- A cursor move or small exposed damage rect no longer forces every unchanged OOP button/control tree to redraw.
- This is the first real retained-window step: child paints update the backing cache; composition reuses it.
- It keeps Win32 semantics: apps still repaint through their existing USER/GDI path, but the compositor owns cached window images like DWM-lite.

Next likely step
- Expand the backing cache contract to in-process app windows once each app exposes a reliable visual sequence/version.
- Then move from per-top-level cache to real per-HWND child/control surfaces and occlusion-aware composition.
