BUILD: myos_v152_user32_windowpos_showwindow_zorder_v1

v152 USER32 WindowPos/ShowWindow/Z-order v1

- Adds IsWindowVisible with parent-chain visibility semantics.
- Hardens ShowWindow return-value and WS_VISIBLE synchronization.
- Hardens SetWindowPos local USER32 geometry/visibility/Z-order semantics.
- Supports HWND_TOP/HWND_BOTTOM for local sibling Z-order.
- Preserves WM_WINDOWPOSCHANGING mutation before commit.
- Adds BeginDeferWindowPos/DeferWindowPos/EndDeferWindowPos lite batching.
- Adds GetWindowPlacement/SetWindowPlacement lite.
- Adds smoke group: ./myos_input --smoke user32_windowpos.
