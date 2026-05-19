# v131 USER32 HWND Access-Control Contract

v131 schiebt zwischen den v130 Thread-/Window-Ownership-Vertrag und späteren MDI-Polish einen expliziten HWND-Zugriffsvertrag.

## Grundsatz

Parent, Owner und Thread-Affinity bleiben getrennt. v131 ergänzt darauf:

- `IsWindow()` und `GetWindowThreadProcessId()` bleiben einfache Identitäts-/Existenzabfragen.
- `CAP_WINDOW_READ` erlaubt Metadaten-Lesen fremder HWNDs.
- Mutierende USER32-Operationen bleiben owner-/thread-affin.
- `CAP_WINDOW_CONTROL` ist kein Freifahrtschein für direkte fremde USER32-Metadatenmutation.
- Cross-owner `PostMessageA`, `SendMessageA` und handgebautes `DispatchMessageA` dürfen keine gefährlichen Messages injizieren.

## Read policy

Fremde HWNDs sind ohne `CAP_WINDOW_READ` für Metadatenabfragen gesperrt:

- `GetWindowTextA`
- `GetClassNameA`
- `GetWindowRect`
- `GetParent`
- `GetWindow(GW_OWNER)`
- `GetWindowLongPtrA`

Mit `CAP_WINDOW_READ` sind diese Wege smoke-gated.

## Mutation policy

Owner-only in v131:

- `SetWindowTextA`
- `EnableWindow`
- `SetWindowLongPtrA`
- `SetParent`
- bereits seit v130: `MoveWindow` / `SetWindowPos` / `DestroyWindow` für USER32-HWNDs

Das ist absichtlich strenger als ein späteres vollständiges Privilege-Modell. Ziel ist, MDI und USER32-Bäume nicht durch fremde direkte Writes zu kontaminieren.

## Message policy

Cross-owner Message-Delivery ist auf request-förmige Fälle begrenzt:

- erlaubt: `WM_CLOSE`
- erlaubt: `WM_SYSCOMMAND/SC_CLOSE`
- blockiert: `WM_SETTEXT`
- blockiert: `WM_COMMAND`
- blockiert: `WM_MDICREATE`
- blockiert: `WM_MDIDESTROY`
- blockiert: `WM_MDIACTIVATE`
- blockiert: sonstige beliebige foreign USER32-WndProc-Injection

Auch ein manuell gebautes öffentliches `MSG` darf nicht mehr über `DispatchMessageA()` in eine fremde USER32-WndProc springen.

## MDI impact

MDI profitiert direkt:

- fremdes `WM_MDICREATE` erzeugt kein Kind in einem fremden MDICLIENT
- fremdes `WM_MDIDESTROY` zerstört kein fremdes MDI child
- fremdes `WM_MDIACTIVATE` ändert den aktiven MDI child nicht
- `WM_MDIGETACTIVE` bleibt owner-local und wird nicht als Cross-Process-Read-Kanal freigegeben

## Bewusst offen

v131 ist kein vollständiges NT-Security-Modell. Noch offen für spätere Versionen:

- echte Tokens/SIDs
- Security Descriptors/DACL/ACE
- WindowStation/Desktop Security
- Cross-thread `SendMessage` mit echter blockierender Reply-Synchronisation
- differenzierte UIAccess-/Privilege-Policy
- vollständige `AttachThreadInput`-Semantik
