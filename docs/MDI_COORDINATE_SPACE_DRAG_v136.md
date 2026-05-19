# v136 MDI Coordinate Space / Drag Fix

The v135 manual test showed the right failure shape: MDI children existed and
could be activated, but hit testing lagged behind the drawn UI and child caption
movement was missing.

Root cause:

- `mdilab_blit()` and `MyDrawChildWindows()` draw app controls relative to the
  WindowManager frame client origin: `frame.x + 1`, `frame.y + TITLEBAR_H`.
- `ScreenToClient()` for WindowManager-backed top-level USER32 HWNDs still used
  the raw USER32 top-level metadata origin.
- Recursive hit testing therefore mixed two coordinate spaces. Buttons and MDI
  children were visually in one place but hit-tested in another.

Fix:

- `mywin_client_origin_screen()` now recognizes WindowManager-backed top-level
  USER32 HWNDs and returns the framed client origin for those HWNDs.
- The generic USER32 fallback remains unchanged for standalone top-level HWNDs
  not owned by a WindowManager frame.
- `DefMDIChildProcA()` now treats negative client Y in the caption band as the
  MDI child move hit zone and captures the mouse.
- Mouse movement reconstructs the screen point from child client coordinates,
  converts it into MDICLIENT coordinates exactly once, and clamps the child to
  the MDICLIENT bounds until MDI scrollbars exist.

Smoke:

- Existing MDI checks remain green.
- New MDI caption-drag tripwire verifies that caption-band input moves an MDI
  child in MDICLIENT-relative coordinates.
