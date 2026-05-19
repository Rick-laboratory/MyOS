BUILD: myos_v127_controls_contract_pass

Ziel von v127:
  Standard-Control-Verträge smoke-gaten, bevor Apps/Labs wieder geradegezogen werden.

Aufbauend auf:
  v122  USER32 geometry contract
  v123  public MSG ABI contract
  v124  lifecycle/message-order contract
  v125  focus/capture/dialog-core contract
  v126  menus/accelerators/modal-loop contract

Neu:
  - neue Smoke-Gruppe: controls
  - BUTTON / CHECKBOX / RADIOBUTTON
  - STATIC
  - EDIT
  - LISTBOX
  - COMBOBOX
  - SCROLLBAR

Smoke-Coverage controls:
  - CreateWindowExA für Standard-Control-Klassen
  - BUTTON BM_CLICK -> WM_COMMAND / BN_CLICKED
  - BM_GETCHECK / BM_SETCHECK
  - AUTOCHECKBOX Toggle
  - AUTORADIOBUTTON Exclusivity
  - STATIC SetWindowText/GetWindowText
  - STATIC WM_GETDLGCODE -> DLGC_STATIC
  - EDIT initial text / WM_CHAR / Backspace / readonly
  - EDIT EN_CHANGE Notification
  - LISTBOX LB_ADDSTRING / LB_GETCOUNT / LB_GETTEXT
  - LISTBOX LB_SETCURSEL / LB_GETCURSEL
  - LISTBOX Keyboard-Auswahl -> LBN_SELCHANGE
  - COMBOBOX CB_ADDSTRING / CB_GETCOUNT / CB_GETLBTEXT
  - COMBOBOX CB_SETCURSEL / CB_GETCURSEL
  - COMBOBOX Keyboard-Auswahl -> CBN_SELCHANGE
  - COMBOBOX CB_SHOWDROPDOWN / CB_GETDROPPEDSTATE
  - SCROLLBAR SBM_SETRANGE / SBM_GETRANGE
  - SCROLLBAR SBM_SETPOS / SBM_GETPOS
  - SCROLLBAR Keyboard -> WM_VSCROLL

Zusätzlich:
  - SDK-Typo in LBS_DISABLENOSCROLL korrigiert.
  - Smoke-WndProc zeichnet jetzt WM_HSCROLL/WM_VSCROLL und WM_COMMAND lParam mit auf.

Build/Test:
  make clean && make -j2
  ./myos_input --smoke all

Erwartung:
  BUILD: myos_v127_controls_contract_pass
  SMOKE RESULT: PASS (0 failures)

Wichtig:
  v127 macht noch keinen vollständigen Common-Control-Stack. Es ist der Vertragsschutz für die klassischen USER32-Controls, damit Calc/DialogLab/ControlLab später nicht mehr gegen zufälliges Altlab-Verhalten repariert werden.
