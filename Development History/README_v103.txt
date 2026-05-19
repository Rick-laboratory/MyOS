myOS v103 - ChooseFontA Common Dialog Core

Adds COMDLG32-lite ChooseFontA on top of the existing USER32/Dialog/Control stack.

Implemented public API surface:
- LOGFONTA
- CHOOSEFONTA
- ChooseFontA
- CF_SCREENFONTS, CF_INITTOLOGFONTSTRUCT, CF_EFFECTS, CF_LIMITSIZE, CF_USESTYLE
- CF_FIXEDPITCHONLY / CF_FORCEFONTEXIST-friendly fixed internal font list
- CFERR_NOFONTS and CFERR_MAXLESSTHANMIN via CommDlgExtendedError()

Dialog implementation uses:
- DialogBoxIndirectParamA
- #32770 / DefDlgProcA
- LISTBOX for font/style/size
- COMBOBOX for color
- BUTTON checkboxes for underline/strikeout
- STATIC preview/status

DialogLab adds a ChooseFontA probe button next to GetOpenFileNameA/GetSaveFileNameA.

Build:
  make clean && make

Run:
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3
