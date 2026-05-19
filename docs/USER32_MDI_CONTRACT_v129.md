# v129 USER32 MDI Child Ownership / Command Routing Contract

v129 fuehrt den ersten echten MDI-Unterbau ein. Vorher gab es keinen belastbaren `MDICLIENT`-Vertrag; MDI war als naechster sichtbarer App-/Lab-Brocken markiert, aber nicht als eigener USER32-Smoke-Bereich abgesichert.

## SDK-Erweiterung

`winuser.h` enthaelt jetzt:

- `CLIENTCREATESTRUCT`
- `MDICREATESTRUCTA`
- `WM_MDICREATE`
- `WM_MDIDESTROY`
- `WM_MDIACTIVATE`
- `WM_MDINEXT`
- `WM_MDIGETACTIVE`
- `WM_MDISETMENU`
- `WM_CHILDACTIVATE`
- `MDIS_ALLCHILDSTYLES`
- `DefFrameProcA`
- `DefMDIChildProcA`

Damit kann Test-/App-Code MDI ueber Win32-aehnliche Header schreiben, ohne private myOS-Hooks.

## USER32-Implementierung

Die Builtin-Klasse `MDICLIENT` wird zusammen mit den Standard-Controls registriert.

Ein `MDICLIENT` speichert intern:

- aktives MDI Child
- Window-Menu Handle aus `CLIENTCREATESTRUCT`
- `idFirstChild`-Sequenz
- fortlaufende Child-ID

MDI Children bleiben normale USER32 HWNDs:

- `GetParent(child) == hwndMdiClient`
- `IsWindow(child)` funktioniert
- `GetDlgCtrlID(child)` bekommt die `idFirstChild`-Sequenz
- `DestroyWindow(MDICLIENT)` zerstoert Children zuerst
- `GetWindowRect` / `MoveWindow` funktionieren ueber den bestehenden Geometrie-Vertrag aus v122

## Nachrichtenvertrag

Smoke-gated in v129:

- `WM_MDICREATE` erzeugt ein echtes Child HWND
- neues Child wird aktiv
- `WM_MDIGETACTIVE` liefert das aktive Child
- `WM_MDIACTIVATE` aktiviert explizit ein Child
- `WM_MDINEXT` rotiert das aktive Child
- `WM_MDIDESTROY` zerstoert ein Child
- Destroy des aktiven Childs faellt auf ein verbleibendes Child zurueck
- `WM_CHILDACTIVATE` und `WM_MDIACTIVATE` werden an Children gesendet

## Command Routing

`DefFrameProcA(frame, mdiClient, WM_COMMAND, ...)` routet an das aktive Child.

Der erste Contract ist bewusst konservativ:

- Child bekommt `WM_COMMAND`/`WM_SYSCOMMAND`
- Rueckgabewert != 0 bedeutet behandelt
- unbehandelter Command faellt zurueck auf `DefWindowProcA`

Das reicht, damit Menue-/Accelerator-Commands aus v126 nicht am Frame kleben bleiben, sondern beim aktiven MDI Child landen koennen.

## Smoke-Gruppe

Neue Gruppe:

```text
./myos_input --smoke mdi
```

Erwartung:

```text
mdi summary :: checks=28 pass=28 fail=0 warn=0
```

Die Gruppe ist bewusst ein Contract-Tripwire, keine vollstaendige MDI-Conformance-Suite.

## Noch offen

Nicht Teil von v129:

- `WM_MDITILE`
- `WM_MDICASCADE`
- `WM_MDIICONARRANGE`
- echtes Window-Menu-Merge
- MDI-Maximize/Restore mit Frame-Menu-Merge
- Scrollbars im MDICLIENT
- vollstaendiges Clipping/MinMax-Verhalten fuer MDI Children

Diese Punkte gehoeren in einen spaeteren MDI-Polish-Pass.
