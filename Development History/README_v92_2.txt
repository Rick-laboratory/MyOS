myos_v92_2_common_controls_hover_mousewheel

Fix over v92.1:
- WM_MOUSEWHEEL is now routed by hover hit-test (WindowFromPoint-style)
  instead of wm_client_endpoint_at_focus().
- LISTBOX/COMBOBOX/SCROLLBAR under the cursor receive wheel messages even
  when another top-level window owns focus.
- Owned top-level dialogs are checked globally for wheel hit-tests, not only
  through the foreground owner HWND.
- MyTopLevelDialogHitTest now checks child controls before clipping to the
  dialog rectangle, so an opened COMBOBOX dropdown can be hit if it extends
  below the normal #32770 frame.

Build:
  make clean && make
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Recommended test:
  Start -> DialogLab -> Open Controls Dialog
  Move focus to another window, hover LISTBOX/COMBOBOX/SCROLLBAR, use wheel.
