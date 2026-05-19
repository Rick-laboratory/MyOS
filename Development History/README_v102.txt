myOS v102 - common dialogs open/save core

BUILD: myos_v102_common_dialogs_open_save_core

Adds a COMDLG32-style Open/Save file dialog core on top of the existing USER32 stack.

Public APIs added:
- OPENFILENAMEA
- GetOpenFileNameA
- GetSaveFileNameA
- CommDlgExtendedError

Implemented behavior in this milestone:
- Win32 double-NUL filter parsing, e.g. "Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0"
- lpstrFile / nMaxFile result copy with FNERR_BUFFERTOOSMALL
- lpstrFileTitle / nMaxFileTitle
- nFileOffset / nFileExtension
- nFilterIndex update
- lpstrInitialDir and lpstrTitle
- OFN_FILEMUSTEXIST
- OFN_PATHMUSTEXIST
- OFN_OVERWRITEPROMPT, implemented as a two-press confirmation inside the dialog status line
- OFN_HIDEREADONLY
- OFN_NOCHANGEDIR

Dialog implementation intentionally uses the real myOS Win32 layers:
- DialogBoxIndirectParamA
- #32770 / DefDlgProcA
- EDIT, LISTBOX, COMBOBOX, BUTTON, STATIC
- WM_COMMAND notifications
- DM_SETDEFID / default button
- IsDialogMessageA keyboard navigation

DialogLab additions:
- GetOpenFileNameA probe
- GetSaveFileNameA probe
- status/dump output for selected file and CommDlgExtendedError

Build/test:
    make clean && make
    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3
