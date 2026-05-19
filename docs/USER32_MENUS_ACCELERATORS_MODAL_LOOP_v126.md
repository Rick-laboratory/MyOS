# myOS v126 USER32 Menus / Accelerators / Modal-Loop Contract

v126 baut auf v122-v125 auf:

- v122: Geometrie / `GetWindowRect` / `MoveWindow`
- v123: public `MSG` ohne private myOS-Felder
- v124: Create/Destroy/Close/Queue/Lifecycle
- v125: Focus / Capture / Dialog-Core

Der Zweck von v126 ist nicht, ein vollständig gezeichnetes Windows-Menüsystem zu liefern. Der Zweck ist, die USER32-Vertragsflächen zu härten, die Menüs, Accelerators und modale Menüloops später brauchen.

## TrackPopupMenu

Vor v126 war `TrackPopupMenu` im Kern ein deterministischer Shortcut: erstes invokable Item suchen, optional Command zurückgeben, sonst `WM_COMMAND` senden.

Ab v126 ist daraus ein USER32-lite Modal-Loop-Vertrag geworden:

- `WM_ENTERMENULOOP`
- `WM_INITMENU`
- `WM_INITMENUPOPUP`
- `WM_MENUSELECT` für Auswahl
- optional `WM_COMMAND`
- `WM_MENUSELECT` mit `HIWORD(wParam)=0xFFFF` beim Close
- `WM_UNINITMENUPOPUP`
- `WM_EXITMENULOOP`

`TPM_RETURNCMD` dispatcht bewusst keinen `WM_COMMAND`; der Command wird direkt zurückgegeben.

Ohne `TPM_RETURNCMD` wird genau ein `WM_COMMAND` an den Owner gesendet.

## Cancel / disabled-only

Ein Popup mit nur disabled/separator Items liefert keinen Command. v126 setzt dafür `ERROR_CANCELLED` und garantiert, dass kein `WM_COMMAND` ausgelöst wird.

Das ist kein vollständiges interaktives Cancel-Modell, aber ein wichtiger Tripwire: disabled Menu Items dürfen nicht versehentlich App-Commands auslösen.

## Focus / Capture Restore

v125 hat Focus/Capture bereits gehärtet. v126 nutzt diesen Boden und schützt die Menü-Schleife:

- vorherige Capture wird gemerkt
- Menüowner kann temporär Capture bekommen
- danach wird alte Capture wiederhergestellt oder freigegeben
- Focus bleibt stabil bzw. wird auf den alten Focus zurückgesetzt

Das verhindert die alte Klasse von Bugs, bei denen Menüs nach Escape/Cancel den Desktop zwar „resumebar“ machen, aber Focus/Capture/Queue vergiften.

## Owner-draw Menu Tripwire

Für `MF_OWNERDRAW` Items ruft der Lite-Pfad jetzt:

- `WM_MEASUREITEM`
- `WM_DRAWITEM`

Das ist noch kein vollständiges OwnerDraw-Menüsystem. Es ist ein Smoke-Tripwire, damit die OwnerDraw-Vertragsroute nicht wieder verschwindet.

## Accelerators

v126 erweitert `TranslateAcceleratorA`:

- `WM_KEYDOWN`
- `WM_SYSKEYDOWN`
- `FCONTROL`
- `FSHIFT`
- `FALT`
- exakter Modifier-Match
- normale Commands gehen als `WM_COMMAND`
- systemische Commands wie `SC_CLOSE` gehen als `WM_SYSCOMMAND`

Das SDK ergänzt dafür:

```c
#define FSHIFT 0x04u
#define FALT   0x10u
```

Außerdem wurde `ERROR_CANCELLED` ergänzt:

```c
#define ERROR_CANCELLED 1223u
```

## Smoke Coverage

Die `menu` Smoke-Gruppe prüft ab v126 unter anderem:

- `CreateMenu`
- `CreatePopupMenu`
- `AppendMenuA`
- `GetMenuItemCount`
- `GetSubMenu`
- `GetMenuItemID`
- `GetMenuItemInfoA`
- `CheckMenuItem`
- `EnableMenuItem`
- `SetMenu`
- `GetMenu`
- `DrawMenuBar`
- `TrackPopupMenu(TPM_RETURNCMD)`
- `TPM_RETURNCMD` ohne `WM_COMMAND`
- `WM_ENTERMENULOOP` / `WM_EXITMENULOOP`
- `WM_INITMENU` / `WM_INITMENUPOPUP` / `WM_UNINITMENUPOPUP`
- `WM_MENUSELECT` close notification
- Capture/Focus Restore
- disabled-only cancel
- OwnerDraw `WM_MEASUREITEM` / `WM_DRAWITEM`
- `TranslateAcceleratorA` Ctrl / Shift / Alt
- Alt+F4 -> `WM_SYSCOMMAND/SC_CLOSE`
- `DestroyAcceleratorTable` lifetime

Ergebnis:

```text
menu summary :: checks=44 pass=44 fail=0 warn=0
SMOKE RESULT: PASS (0 failures)
```

## Bewusste Grenzen

v126 ist immer noch kein echter Windows-Menümanager:

- keine echte Popup-Zeichnung
- keine echte Hover-Navigation
- kein vollständiges Submenu-Tracking mit Mauskoordinaten
- keine komplette `TrackPopupMenuEx` / `MENUINFO` / `MENUITEMINFO`-Breite
- keine vollständige systemweite Menu-Lifetime

Aber der wichtigste Vertrag ist jetzt geschützt: Menüloops dürfen Focus, Capture, Queue und Owner-Command-Dispatch nicht mehr still kaputtmachen.
