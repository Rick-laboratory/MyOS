myOS v176 - scoped window damage compositor
BUILD: myos_v176_scoped_window_damage

Architectural step after v175 damage rects:
- v175 introduced compositor damage rectangles, framebuffer clipping, and partial flips.
- v176 classifies visible scene deltas and damages old+new top-level window/icon/menu rectangles instead of forcing full-screen damage for every non-pointer state change.
- OOP-GDI / control/button updates now dirty their owning top-level window rectangle, not the whole desktop.
- Window move/resize, terminal changes, apphost GDI sequence changes, focus/title/minimize changes and icon changes are scoped.
- Background/layout/wallpaper changes remain conservative full-damage because they alter all uncovered desktop pixels.

This is still not final DWM/occlusion-tree composition; it is the next safe retained-compositor step toward per-HWND backing stores and per-control dirty regions.
