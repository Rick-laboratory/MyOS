# v130 USER32 Thread / Window Ownership Contract

v130 haertet den Unterbau, den MDI spaeter zwingend braucht. Der wichtige Schritt ist die Trennung von drei Win32-Dimensionen, die vorher faktisch in `hParent` und `cap.id` zusammengefallen sind.

## Getrennte Dimensionen

`MyWinWindowInfo` fuehrt nun getrennt:

- `hParent`: visuelle/Clipping-Hierarchie fuer `WS_CHILD` Fenster
- `hOwner`: Owner/Z-Order/Modal-Beziehung fuer Top-Level/Popup-Fenster
- `dwProcessId`: Prozess-/Capability-Owner des HWND
- `dwThreadId`: UI-Thread-Affinity des HWND

Der PoC nutzt weiterhin `pid == tid` pro Capability, aber USER32-Code greift nicht mehr implizit darauf zu. Damit kann spaeter ein echtes Threadmodell nachgezogen werden, ohne Parent/Owner/Focus/MDI erneut umzubauen.

## Public API / Vertrag

Neu bzw. gehaertet:

- `GetWindowThreadProcessId(HWND, LPDWORD)`
- `AttachThreadInput(DWORD, DWORD, BOOL)` als Lite-Input-Queue-Kopplung
- `GetParent()` gibt nur `hParent` fuer Child-Windows zurueck
- `GetWindow(GW_OWNER)` gibt `hOwner` zurueck
- `GWLP_HWNDPARENT` bleibt Win32-kompatibler Legacy-Zugriff:
  - Child: Parent
  - Top-Level: Owner

## Security-/Ownership-lite

`CAP_WINDOW_CONTROL` ist nicht mehr genug, um fremde USER32-HWNDs direkt zu kontrollieren.

Smoke-gated:

- Fremder Thread darf eigenes Top-Level-Fenster erzeugen
- Fremder Thread darf kein `WS_CHILD` unter fremden Parent haengen
- Fremder Thread darf fremdes USER32-Child nicht per `MoveWindow` bewegen
- Fremder Thread darf fremdes USER32-Child nicht per `DestroyWindow` zerlegen
- `SetFocus` auf fremdes HWND scheitert ohne `AttachThreadInput`
- `AttachThreadInput(TRUE)` koppelt Input-Queues fuer Focus-lite
- `SetCapture` bleibt an Thread-/Input-Ownership gebunden

## MDI-Invariante

MDI ist jetzt strenger:

- `MDICLIENT` und MDI-Child muessen denselben Window-Owner haben
- `WM_MDIACTIVATE` weist fremde/inkompatible Children ab
- `WM_MDICREATE` laeuft nur aus dem Owner-Kontext des `MDICLIENT`

Das verhindert die klassischen Spaetfehler:

- Frame routet `WM_COMMAND` an falsches Child
- Focus zeigt auf fremde Queue
- Destroy/Move zerlegt fremden UI-Baum
- Active-MDI-State und ThreadQueue laufen auseinander

## Lock-Order-Notiz

v130 fuehrt keine neue globale Lock-Hierarchie ein. Wichtig fuer kommende Cross-Thread-SendMessage-Arbeit:

- USER32-Metadaten (`g_WindowInfos`) duerfen nicht unter `HWNDManager.lock` in fremde WNDPROCs dispatchen.
- `hwnd_send_timeout()` bleibt der Ort fuer den Queue-/Sync-Send-Pfad.
- Der kommende echte LRESULT-Rueckkanal fuer foreign USER32-HWNDs muss ohne Lock-Inversion zwischen USER32-Metadaten, ThreadQueue und SyncSendContext gebaut werden.

## Bewusst offen

- Echtes multi-threaded USER32 pro Prozess
- Vollstaendiger Cross-Thread-`SendMessage` LRESULT-Rueckkanal fuer USER32-Metadaten
- Vollstaendige `AttachThreadInput`-Semantik wie Windows
- WindowStation/Desktop/Security-Descriptor-Modell
