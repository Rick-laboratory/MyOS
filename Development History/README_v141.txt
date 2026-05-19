# myOS v141 - MDI subfocus escape + visual caption drag

Built on v140_mdi_nc_child_hit_sync_button_route.

Goals:
- keep the now-working MDILab menubar/menu-command path unchanged;
- keep the v140 synchronous BUTTON down/up route for toolbar controls;
- make MDI child captions a first-class visual hit region for the raw mouse path;
- prevent the generic WM_MOUSEMOVE fallback from racing the raw MDI drag latch;
- route plain Escape to the active MDI child first in this lab/session contract.

Key changes:
- `wm_mdi_visual_caption_hit()` treats the top band of the MDI child window rect as
  an explicit screen-space caption hit region.
- `wm_try_mdi_caption_mouse_down()` now accepts either the child `WM_NCHITTEST`
  result or the explicit visual-caption hit region before starting raw MDI drag.
- `wm_mouse_move()` now returns whether the WindowManager consumed the move.
  The desktop input loop only posts the generic client `WM_MOUSEMOVE` when the
  WindowManager did not consume the move. This prevents raw MDI drag from being
  followed by a second stale client-move fallback.
- The desktop Escape path closes the active MDI child before falling back to
  top-level `SC_CLOSE`.

Smoke additions/coverage:
- MDILab toolbar one-shot stays green.
- Fast toolbar click down/up-before-dispatch stays green.
- New-after-close-all cascade reset stays green.
- Physical MDI caption drag now asserts the raw move is consumed.

Still honest:
- The real VT/evdev hand-test must confirm the manual drag path, because smoke
  can only exercise the in-process raw router.
- This is a pragmatic step toward a later explicit MDI sub-element hit-region
  table for caption, borders, resize grips and caption buttons.
