myOS v87 - common controls / DlgItem / text contract
====================================================

Ziel
----
MSDN-naeherer USER32-Control-Vertrag fuer BUTTON/STATIC/EDIT und Dialog-Item APIs.

Neu/erweitert
-------------
- Built-in class BUTTON erweitert:
  - BM_CLICK
  - BM_GETSTATE / BM_SETSTATE
  - BM_GETCHECK / BM_SETCHECK
  - BM_SETSTYLE-lite
  - BN_CLICKED / BN_SETFOCUS / BN_KILLFOCUS
  - Space/Enter bleiben ueber WM_COMMAND/BN_CLICKED aktiv

- Built-in class STATIC:
  - WM_SETTEXT / WM_GETTEXT / WM_GETTEXTLENGTH
  - sichtbares Rendering via MyDrawChildWindows()

- Built-in class EDIT-lite:
  - WM_SETTEXT / WM_GETTEXT / WM_GETTEXTLENGTH
  - WM_CHAR text append
  - Backspace via WM_KEYDOWN
  - EN_CHANGE notification an Parent
  - Focus/Caret-Anzeige im Parent-Renderer

- Neue USER32-lite APIs:
  - GetDlgItem
  - GetDlgCtrlID
  - SendDlgItemMessageA
  - SetDlgItemTextA
  - GetDlgItemTextA
  - GetWindowTextLengthA
  - EnableWindow / IsWindowEnabled
  - CheckDlgButton / IsDlgButtonChecked

- DefWindowProcA behandelt jetzt WM_SETTEXT/WM_GETTEXT/WM_GETTEXTLENGTH fuer USER32-lite HWND metadata.

Regression-Schutz
-----------------
- v86.2 START direct command fix bleibt drin.
- v86 Dialog keys / Tab / IsDialogMessage bleiben drin.
- v85 Menu keyboard loop bleibt drin.
- v83/v84 Alt+Space/Alt+F4 Pfade bleiben drin.

Build
-----
make clean && make

Run
---
sudo chvt 3
sudo ./myos_input /dev/input/event1 /dev/input/event2

Testprozedur
------------
1. START klicken -> Startmenue muss oeffnen.
2. ControlLab [OOP child-HWND] oeffnen.
3. Tab / Shift+Tab / Space / Enter testen.
4. Ping/Toggle Buttons muessen WM_COMMAND/BN_CLICKED liefern.
5. STATIC Child-Text sollte sichtbar sein.
6. ServiceLab oeffnen: BUTTON child HWNDs muessen weiter reagieren.
7. Editor/Calc/Spy++ Regression testen.
8. Alt+Space, Alt+F4, Menue Up/Down/Enter/Esc Regression testen.

Hinweise
--------
- EDIT ist bewusst noch EDIT-lite: Singleline, Append/Backspace, kein Selection-Modell.
- Cross-process GetDlgItemText/SetDlgItemText von Linux-Child zu Parent ist noch nicht als eigener IPC-KREQ gebridged.
  Parent/in-process USER32-lite API ist vorhanden; OOP Controls zeichnen und notifizieren bereits ueber parent-owned HWNDs.
