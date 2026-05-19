BUILD: myos_v238_menu_overlay_damage_signature

Fixes transient app-menu popup frontbuffer artifacts by hashing menu overlay content/state into the desktop render signature.

Key points:
- App menu overlay content now participates in render-signature invalidation.
- MDI Window-menu popup text/check/selection/submenu stack changes trigger scoped menu damage even when popup rectangles are unchanged.
- Menu overlays remain compositor-owned chrome; this does not move menu semantics into HWND backing caches.
