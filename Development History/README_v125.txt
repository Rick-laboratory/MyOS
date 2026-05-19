BUILD: myos_v125_focus_capture_dialog_core_contract

Basis:
  myos_v124_user32_lifecycle_message_order_contract

Ziel von v125:
  USER32 Focus/Capture/Dialog-Core als Vertrag härten, bevor alte Labs/Apps
  wieder hübsch gezogen werden.

Wichtigste Core-Fixes:
  - SetFocus sendet WM_KILLFOCUS / WM_SETFOCUS jetzt synchron statt per Queue.
  - SetFocus lehnt disabled HWNDs ab.
  - EnableWindow(FALSE) räumt Focus und Capture für das deaktivierte Fenster
    oder dessen Kinder auf.
  - EnableWindow sendet WM_ENABLE synchron.
  - SetCapture gibt bei erstmaligem Capture NULL zurück, wie USER32.
  - WM_CAPTURECHANGED wird synchron an das Fenster gesendet, das Capture verliert.
  - WM_CAPTURECHANGED nutzt lParam für das neue Capture-HWND; wParam bleibt 0.

Neue Smoke-Gruppe:
  focusdlg

Neue focusdlg-Tripwires:
  - SetFocus erstes Kind -> NULL, GetFocus == Kind
  - WM_SETFOCUS synchron
  - Fokuswechsel Kind A -> Kind B liefert WM_KILLFOCUS / WM_SETFOCUS
  - EnableWindow(FALSE) auf fokussiertem Kind löscht Focus
  - disabled HWND kann nicht fokussiert werden
  - EnableWindow(TRUE) stellt Enabled-State wieder her
  - GetNextDlgTabItem findet erste Tabstop-Control
  - GetNextDlgTabItem überspringt disabled Controls
  - IsDialogMessageA TAB / Shift+TAB
  - IsDialogMessageA ENTER -> default pushbutton / IDOK
  - IsDialogMessageA ESC -> cancel / IDCANCEL
  - Radio-Arrow bewegt Focus und checked state innerhalb einer AutoRadio-Gruppe

Capture-Smoke wurde angepasst:
  - SetCapture(first) erwartet jetzt NULL als Rückgabe.
  - Capture replacement/release erwartet synchrone WM_CAPTURECHANGED-Zustellung.
  - lParam trägt das neue Capture-HWND.

Verifikation:
  make clean && make -j2
  ./myos_input --smoke all

Erwartetes Ergebnis:
  BUILD: myos_v125_focus_capture_dialog_core_contract
  SMOKE RESULT: PASS (0 failures)
