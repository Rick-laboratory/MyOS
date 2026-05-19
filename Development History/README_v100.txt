myOS v100 - dialog manager MSDN compliance core

Basis: myos_v99_dialog_arrow_group_button_fix

Fokus:
- DefDlgProcA / IsDialogMessageA / WM_GETDLGCODE / WM_NEXTDLGCTL / DM_GETDEFID / DM_SETDEFID weiter in Richtung Win32/MSDN-Semantik gehaertet.

Aenderungen:
1) Persistentes Default-Button-Modell
   - MyWinDialogInfo.defId ist jetzt der durch DM_SETDEFID gesetzte dauerhafte Default.
   - focusDefId ist nur der temporaere Default-Visual, wenn der Fokus auf einem Pushbutton liegt.
   - Verlaesst der Fokus die Button-Gruppe, wird der persistente Default wieder visuell hergestellt.

2) DM_GETDEFID / DM_SETDEFID
   - DM_GETDEFID liefert MAKELRESULT(id, DC_HASDEFID), wenn ein Default existiert.
   - DM_SETDEFID setzt den persistenten Default und aktualisiert BS_DEFPUSHBUTTON/BS_PUSHBUTTON-Visuals.

3) Enter/Escape
   - DefDlgProcA sendet Enter nicht mehr stumpf an IDOK, sondern an den aktuellen Default-Button.
   - Escape bleibt IDCANCEL.
   - IsDialogMessageA klickt bei Enter nur fokussierte Pushbuttons direkt; Checkbox/Radio bleiben Space-getrieben.

4) WS_TABSTOP / WS_GROUP
   - Dialog-Tabnavigation respektiert bei BUTTON nun WS_TABSTOP statt jeden Button automatisch als Tabstop zu behandeln.
   - Radio-Gruppen behalten den v99-Pfad: checked Radio ist der Tab-Kandidat; Pfeile bewegen innerhalb der Gruppe.

5) WM_NEXTDLGCTL
   - lParam != 0 setzt den Fokus direkt auf HWND aus wParam.
   - lParam == 0 navigiert next/previous per GetNextDlgTabItem.

6) DialogLab Validierung
   - Neuer Button: Dump Dialog Nav.
   - Dump zeigt pro Child hwnd/id/class/style/WM_GETDLGCODE/Focus und DM_GETDEFID.
   - GetClassNameA als echte Win32-API ergaenzt, damit Tests nicht auf Fake-APIs angewiesen sind.

Build/Test:
  make clean && make
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

DialogLab Tests:
- Open Keyboard Dialog -> Tab/Shift+Tab, Pfeile auf Radio/Buttons, Apply -> WM_NEXTDLGCTL.
- Dump Dialog Nav -> pruefen, dass dlgcode, TABSTOP/GROUP-style und DM_GETDEFID sichtbar sind.
- Enter auf EDIT sollte Default OK ausloesen; Space toggelt Checkbox/Radio; Enter auf Radio/Checkbox nicht.
