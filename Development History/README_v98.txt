myOS v98 - dialog navigation core fix
====================================

Base: myos_v97_2_dialog_tab_arrow_command_fix

Validated/fixed first, before moving further into menus/common dialogs:

1) SCROLLBAR now answers WM_GETDLGCODE with DLGC_WANTARROWS.
   This lets IsDialogMessageA forward arrow/page/home/end keys to focused
   scrollbars, matching the already working LISTBOX/COMBOBOX path.

2) SendMessageA/SendMessageTimeoutA now preserve LRESULT for USER32-created
   windows with a local WNDPROC in MyWinWindowInfo.
   This fixes synchronous API probes such as DM_GETDEFID, WM_GETDLGCODE,
   BM_GETCHECK, LB_GETCURSEL, CB_GETCURSEL, SBM_GETPOS and friends.

3) Existing v97.2 command guard remains intact:
   IDOK/IDCANCEL only close on BN_CLICKED or code 0; BN_SETFOCUS and
   BN_KILLFOCUS are observable notifications and do not EndDialog.

Recommended test path:

    make clean && make
    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

DialogLab checks:

- Open Keyboard Dialog:
  Tab should cycle Name -> Password -> checked radio group representative -> Apply -> OK -> Cancel -> Name.
  Arrow keys on the radio group should move/check One/Two/Three inside the WS_GROUP range.
  Apply calls WM_NEXTDLGCTL and the status line should show a non-zero DM_GETDEFID value.

- Open Controls Dialog:
  Tab to LISTBOX/COMBOBOX/SCROLLBAR.
  Arrow keys should change list selection, combo selection/dropdown navigation, and scrollbar position.

- Open Text/Dialog basic modal:
  Tab onto OK/Cancel must only show focus notifications and must not close the dialog.
  Enter/Space or a mouse click should still click the focused/default button.
