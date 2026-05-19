BUILD: myos_v136_mdi_coordinate_space_drag_fix

v136 fixes the MDILab manual interaction regression reported after v135:

- WindowManager-backed USER32 top-level HWNDs now use the framed client origin
  for ScreenToClient()/nested ChildWindowFromPoint hit testing.
- This aligns hit testing with what the compositor actually draws: x+1,
  y+TITLEBAR_H for app client space.
- MDI child caption clicks now activate and start a bounded drag inside the
  MDICLIENT.
- Drag math reconstructs screen coordinates from child client coordinates and
  converts exactly once into MDICLIENT-relative coordinates.
- Existing v131 HWND access control and v133 shell broker context remain intact.

Manual target:
- MDILab toolbar buttons should be clickable where they are drawn.
- MDI children should be selectable and draggable by their title/caption band.
- Dragged/cascaded children should remain bounded in the MDICLIENT while no
  MDI scrollbars exist.
