# myOS v125 USER32 Focus / Capture / Dialog-Core Contract

v125 baut direkt auf v122-v124 auf:

- v122: Geometrie (`GetWindowRect`, `MoveWindow`, `SetWindowPos`-Grundpfad)
- v123: public `MSG`-ABI ohne private myOS-Felder
- v124: Create/Destroy/Close/Queue/Lifecycle-Reihenfolge

Der Zweck von v125 ist nicht, DialogLab oder Calc kosmetisch zu flicken. Der Zweck ist, die USER32-Grundpfade zu härten, auf denen Dialoge, Controls, Menüs und Apps später zuverlässig laufen müssen.

## Focus Contract

Vor v125 wurden Focus-Notifications über die Queue zugestellt. Das machte Dialognavigation order-abhängig: `GetFocus()` war schon geändert, während `WM_KILLFOCUS` und `WM_SETFOCUS` noch nicht beim Control angekommen waren.

v125 ändert das:

- `SetFocus(hwnd)` prüft, ob das Ziel gültig und enabled ist.
- disabled HWNDs können nicht fokussiert werden.
- `WM_KILLFOCUS` an das alte Focus-HWND wird synchron gesendet.
- `WM_SETFOCUS` an das neue Focus-HWND wird synchron gesendet.
- Rückgabe bleibt USER32-artig: altes Focus-HWND oder `NULL`.

## EnableWindow Contract

`EnableWindow(FALSE)` ist ab v125 nicht mehr nur ein Style-Bit.

Wenn das deaktivierte Fenster oder eines seiner Kinder Focus oder Capture hält, wird dieser Zustand vor Rückkehr der API aufgeräumt:

- Capture wird freigegeben.
- Focus wird auf `NULL` gesetzt.
- `WM_ENABLE` wird synchron gesendet.
- Rückgabe bleibt der vorherige Enabled-Zustand.

Das verhindert später genau die Sorte Fehler, bei denen disabled Dialog-Controls weiter Tastatur- oder Mauszustand halten.

## Capture Contract

Vor v125 war `SetCapture(first)` zu freundlich und gab das neue Capture-HWND zurück. Win32 erwartet aber das vorherige Capture-HWND, also bei erstmaligem Capture `NULL`.

v125 härtet:

- `SetCapture(hwnd)` gibt das vorherige Capture-HWND zurück.
- erstes Capture gibt `NULL` zurück.
- Capture auf disabled HWND wird abgelehnt.
- das Fenster, das Capture verliert, erhält synchron `WM_CAPTURECHANGED`.
- `WM_CAPTURECHANGED.wParam == 0`.
- `WM_CAPTURECHANGED.lParam == neues Capture-HWND` oder `NULL` bei `ReleaseCapture`.

## Dialog-Core Tripwires

Die neue Smoke-Gruppe `focusdlg` deckt die wichtigsten Dialog-Core-Achsen ab:

- `SetFocus` / `GetFocus`
- `WM_SETFOCUS` / `WM_KILLFOCUS`
- `EnableWindow` und Focus-Clearing
- `GetNextDlgTabItem`
- disabled Tabstop-Skip
- `IsDialogMessageA` für `TAB` und `Shift+TAB`
- `ENTER` zum Default-Pushbutton (`IDOK`)
- `ESC` zum Cancel-Pushbutton (`IDCANCEL`)
- AutoRadio-Arrow-Navigation inklusive checked state

Das ist bewusst noch keine vollständige Dialog-Conformance-Suite. Es ist ein Refactor-Tripwire für die Pfade, die bisher manuell in DialogLab aufgefallen sind.

## Erwartete Smoke-Zusammenfassung

```text
capture   summary :: checks=11 pass=11 fail=0 warn=0
focusdlg  summary :: checks=23 pass=23 fail=0 warn=0
SMOKE RESULT: PASS (0 failures)
```

Alle alten Smoke-Gruppen bleiben grün.
