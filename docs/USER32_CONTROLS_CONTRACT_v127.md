# myOS v127 USER32 Controls Contract Pass

v127 baut auf v122-v126 auf. Nachdem Geometrie, public `MSG`, Lifecycle, Focus/Capture/Dialog-Core und Menüs/Accelerators smoke-gated sind, wird jetzt die klassische USER32-Control-Familie als eigener Vertrag geprüft.

## Zweck

Die alten Apps/Labs sollen nicht mehr die einzige Quelle sein, um Control-Regressions zu sehen. `controls` ist jetzt eine eigene Smoke-Gruppe. Sie prüft nicht die komplette Windows-Conformance, sondern die Verhaltensflächen, auf denen Calc, DialogLab, ControlLab und Common-Dialog-ähnliche UIs später aufsetzen.

## Neue Smoke-Gruppe

```text
./myos_input --smoke controls
```

Die Gruppe erzeugt ein normales Parent-Fenster und Standard-Controls als Child-HWNDs. Der Parent-WndProc zeichnet `WM_COMMAND`, `WM_HSCROLL` und `WM_VSCROLL` auf. Dadurch werden nicht nur Rückgabewerte geprüft, sondern auch echte Parent-Notifications.

## BUTTON

Geprüft wird:

- `CreateWindowExA("BUTTON", ...)`
- `BM_CLICK`
- `WM_COMMAND` an Parent
- `LOWORD(wParam) == control id`
- `HIWORD(wParam) == BN_CLICKED`
- `lParam == control HWND`
- `BM_GETCHECK` / `BM_SETCHECK`
- `BS_AUTOCHECKBOX` Toggle
- `BS_AUTORADIOBUTTON` Gruppenausschluss

Das ist der Pfad, den Calc und Dialog-Buttons später stabil brauchen.

## STATIC

Geprüft wird:

- `CreateWindowExA("STATIC", ...)`
- `SetWindowTextA` / `GetWindowTextA`
- `WM_GETDLGCODE -> DLGC_STATIC`

STATIC bleibt damit ein nicht-fokussierbares Beschriftungs-/Anzeige-Control, solange kein späterer expliziter Sonderstil implementiert wird.

## EDIT

Geprüft wird:

- Initialer Text aus `lpWindowName`
- `WM_GETTEXT`
- `EM_SETSEL`
- `WM_CHAR`
- `EN_CHANGE`
- `WM_KEYDOWN / KEY_BACKSPACE`
- `EM_SETREADONLY`

Wichtig: Die Smoke-Gruppe leert bewusst die Parent-Queue zwischen Edit-Mutationen, damit spätere Control-Notifications nicht durch alte `EN_UPDATE`/`EN_CHANGE`-Messages verfälscht werden.

## LISTBOX

Geprüft wird:

- `LB_ADDSTRING`
- `LB_GETCOUNT`
- `LB_GETTEXT`
- `LB_SETCURSEL`
- `LB_GETCURSEL`
- Keyboard-Auswahl über `WM_KEYDOWN / KEY_UP`
- `LBN_SELCHANGE` an Parent bei `LBS_NOTIFY`

Das schützt den Pfad, der bei Dialog-Labs und Common-Dialog-Listen später besonders wichtig ist.

## COMBOBOX

Geprüft wird:

- `CB_ADDSTRING`
- `CB_GETCOUNT`
- `CB_GETLBTEXT`
- `CB_SETCURSEL`
- `CB_GETCURSEL`
- Keyboard-Auswahl über `WM_KEYDOWN / KEY_DOWN`
- `CBN_SELCHANGE` an Parent
- `CB_SHOWDROPDOWN`
- `CB_GETDROPPEDSTATE`

Das ist noch kein vollständiger ComboBox-Edit/List-Popup-Stack, aber der zentrale Auswahl- und Notification-Vertrag ist jetzt abgesichert.

## SCROLLBAR

Geprüft wird:

- `SBM_SETRANGE`
- `SBM_GETRANGE`
- `SBM_SETPOS`
- `SBM_GETPOS`
- Keyboard-Steuerung über `WM_KEYDOWN / KEY_DOWN`
- `WM_VSCROLL` an Parent

## SDK-Korrektur

`LBS_DISABLENOSCROLL` hatte einen kaputten Makro-Text:

```c
#define LBS_DISABLENOSCROLL 0x1000u 0x1000u
```

v127 korrigiert das auf:

```c
#define LBS_DISABLENOSCROLL 0x1000u
```

## Nicht-Ziel von v127

v127 ist noch keine vollständige Controls-Conformance-Suite. Bewusst noch nicht vollständig abgedeckt:

- EDIT Undo/LimitText/Clipboard
- OwnerDraw LISTBOX/COMBOBOX Vollpfad
- vollständige ComboBox-Edit-Child-Semantik
- Scrollbar Thumb-Tracking mit Mouse-Capture als Conformance-Test
- komplette Button-Default-Visual-Semantik

Das kommt sinnvollerweise nach der App/Lab-Reparaturrunde oder als gezielte v12x-Control-Vertiefung.
