myos_v95_v96_dialog_keyboard_scrollinfo

v95:
- hardened IsDialogMessageA for Tab/Shift+Tab, Alt mnemonics, radio-group arrow navigation, Enter/Esc routing, WM_GETDLGCODE awareness
- added DM_GETDEFID / DM_SETDEFID and WM_NEXTDLGCTL handling in DefDlgProcA
- added Keyboard/Nav Probe dialog in DialogLab

v96:
- added WS_VSCROLL / WS_HSCROLL constants
- added SCROLLINFO, SetScrollInfo, GetScrollInfo, ShowScrollBar, EnableScrollBar
- added standard scrollbar drawing overlay for top-level app client areas
- DialogLab can enable/test standard scrollbars on itself
