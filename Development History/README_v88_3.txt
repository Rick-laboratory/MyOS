myOS v88.3 - dialog edit refocus fix
=====================================

Basis: myos_v88_2_dialog_top_level_escape_close

Fix:
- EDIT controls now take focus on WM_LBUTTONDOWN.
- This fixes the DialogLab regression where clicking outside the input field
  lost focus and clicking back into the EDIT did not restore typing; only Tab
  could re-enter the input.

Expected DialogLab behavior:
- Click EDIT: caret/focus returns immediately.
- Type text after clicking back in.
- Tab traversal still works.
- OK/Cancel mouse and Enter/Esc behavior from v88.2 remain unchanged.
