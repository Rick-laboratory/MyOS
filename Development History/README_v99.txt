myOS v99 - dialog arrow/group button fix
========================================

Basis: myos_v98_dialog_nav_core_fix

Fokus dieser Version:
- IsDialogMessageA folgt jetzt dem Win32-Dialog-Keyboard-Modell fuer Pfeiltasten auch dann,
  wenn das fokussierte Control kein DLGC_WANTARROWS meldet.
- LEFT/UP gehen per GetNextDlgGroupItem zum vorherigen Control in der Gruppe.
- RIGHT/DOWN gehen per GetNextDlgGroupItem zum naechsten Control in der Gruppe.
- DLGC_STATIC-Ziele werden uebersprungen; fokussierbare Buttons/Controls bekommen Fokus.
- BS_AUTORADIOBUTTON wird beim Pfeilwechsel per BM_CLICK aktiviert.
- Auto-Radio-Buttons verschieben WS_TABSTOP innerhalb der Radio-Gruppe auf den aktuell
  gewaehlten Button, damit Tab spaeter wieder beim zuletzt gewaehlten Radio landet.
- DialogLab-Templates markieren den ersten Pushbutton nach einer Radio-Gruppe mit WS_GROUP,
  damit Radio-Gruppen und Pushbutton-Gruppen sauber getrennt sind.

Build-Test:
    make clean && make

Manueller Test:
    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

DialogLab Checks:
- Open Keyboard Dialog:
  * Tab: EDIT -> One/checked radio -> Apply -> OK -> Cancel -> EDIT
  * Pfeile auf Radio One/Two/Three: wechselt Radio und Check-State.
  * Pfeile auf Apply/OK/Cancel: Fokus wandert innerhalb der Button-Gruppe.
- Open Button Dialog:
  * Pfeile auf Radio-Buttons wechseln Auswahl.
  * Pfeile auf Push/OK/Cancel wechseln Fokus in der Pushbutton-Gruppe.
- Listbox/ComboBox/Scrollbar bleiben wie v98: sie bekommen Pfeile weiterhin synchron,
  weil sie DLGC_WANTARROWS melden.
