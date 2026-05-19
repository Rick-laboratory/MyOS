myos_v95_v96_dialog_keyboard_scrollinfo

Build on v93.1 BUTTON/Common Controls and hardens the classic STATIC/EDIT families.

Highlights:
- Fixed stale desktop status label from v88 to v94.
- STATIC styles: SS_LEFT, SS_CENTER, SS_RIGHT, SS_ICON placeholder, SS_BLACK/GRAY/WHITE RECT/FRAME, SS_ETCHEDHORZ, SS_ETCHEDVERT, SS_ETCHEDFRAME, SS_CENTERIMAGE, SS_NOPREFIX.
- EDIT styles: ES_LEFT, ES_CENTER, ES_RIGHT, ES_PASSWORD, ES_READONLY, ES_MULTILINE, ES_AUTOHSCROLL, ES_AUTOVSCROLL, ES_WANTRETURN, ES_NUMBER, ES_UPPERCASE, ES_LOWERCASE.
- EDIT caret movement: Left/Right/Home/End, Backspace/Delete, multiline Up/Down and Enter.
- Selection-lite: mouse drag selection, EM_SETSEL, EM_GETSEL, EM_REPLACESEL.
- Line helpers: EM_GETLINECOUNT, EM_LINEFROMCHAR, EM_LINEINDEX, EM_LINELENGTH, EM_GETLINE, EM_GETFIRSTVISIBLELINE, EM_LINESCROLL, EM_SCROLLCARET.
- Password/read-only helpers: EM_SETPASSWORDCHAR, EM_GETPASSWORDCHAR, EM_SETREADONLY.
- Parent notifications: EN_SETFOCUS, EN_KILLFOCUS, EN_UPDATE, EN_CHANGE.
- DialogLab now has Open Text Dialog with a DLGTEMPLATE-built Static/Edit Probe.

Test:
make clean && make
sudo chvt 3
sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

Open DialogLab -> Open Text Dialog.
