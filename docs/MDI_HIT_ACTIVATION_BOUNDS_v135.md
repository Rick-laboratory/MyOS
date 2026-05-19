# v135 MDI Hit / Activation / Bounds Fix

Manual MDILab testing after v134 exposed the important gap that the existing smoke tests did not cover: the MDI core could create and arrange child HWNDs, but raw mouse routing only hit the direct child of the frame. In MDILab that direct child was the MDICLIENT, so clicks did not reach the nested MDI child windows.

## Fixes

- Added recursive/deep client hit-testing in the compositor endpoint path so nested USER32 children can receive mouse input.
- Added MDI child activation on `WM_LBUTTONDOWN`, `WM_NCLBUTTONDOWN`, and `WM_SETFOCUS` through `DefMDIChildProcA`.
- Added MDICLIENT-side hit activation for clicks that still land on the client.
- Added idFirstChild window-menu command handling before normal frame command routing.
- Added owner-context entry for compositor app-menu command sends, so v131 access control stays strict while app menus still operate as the app owner.
- Reworked cascade layout to keep child rectangles inside the MDICLIENT when no MDI scrollbars exist yet.
- MDILab now draws active children with a visible active marker.

## Smoke additions

The MDI smoke group now gates:

- Window-menu child command activation
- Mouse activation of MDI child HWNDs
- Cascade children staying inside the MDICLIENT bounds

