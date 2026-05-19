myOS v142 - MDI activation Z-order contract

Goal:
- Keep v141 physical MDI caption drag working.
- Make MDI activation raise the child in the MDICLIENT child Z-order.
- Align hit-test, GetWindow(GW_*), EnumChildWindows/ChildWindowFromPoint, drawing and Window-menu ordering with USER32-local child Z-order.

Manual symptom fixed:
- Clicking a background MDI child no longer only marks it active; it is raised visually to the top of the cascade stack.

Build:
  make clean && make -j2
  ./myos_input --smoke all
