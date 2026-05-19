myOS v88.2 - dialog top-level + Escape close semantics

Fixes over v88.1:
- #32770 dialogs are now owned top-level USER32-lite windows instead of WS_CHILD controls clipped inside the parent client.
- Dialogs render globally above app windows via MyDrawTopLevelDialogs().
- Mouse routing can hit top-level dialogs and their child controls outside the owner window.
- Dialog drag moves in desktop/screen coordinates instead of clamping to owner.
- Keyboard focus accepts owned dialog children even while the modal parent is disabled.
- ESC no longer terminates myOS while a window is focused. It closes the foreground window one by one; only when no window is focused does ESC exit the OS.
