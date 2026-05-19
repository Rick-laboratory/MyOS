myOS v172 - owned OOP dialog destroy damage

Goal: keep the v171 WM_COMMAND activation filter, but fix the architectural repaint hole seen after closing an OOP modal DialogLab dialog.

Changes:
- IPC CreateWindowExA now distinguishes WS_CHILD parentage from top-level ownership. A non-child hWndParent is carried as owner_hwnd to the parent broker.
- OOP DialogLab modal probes are created as owned top-level popup windows using the DialogLab HWND as owner.
- Parent DestroyWindow broker captures owner/parent before destruction and RedrawWindow()s the exposed owner subtree after successful destroy.
- USER32 DestroyWindow has a generic owner/parent expose damage path. This is not DialogLab-specific.
- EnableWindow invalidates the target and descendants after WM_ENABLE because effective child enabled visuals depend on the parent chain.
- Child retained GDI streams drop commands for destroyed OOP dialog HWNDs and their child controls before the owner repaint is published.

MSDN direction:
- WS_CHILD parent and top-level owner are separate relationships.
- Destroying/hiding an owned popup exposes and invalidates the owner/underlying area.
- EnableWindow changes visible state and must create paint damage instead of waiting for incidental repaint.
