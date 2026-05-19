# v128 App/Lab Repair Against Contract

v128 zieht die App-/Lab-Canaries nach den Vertragsarbeiten aus v122-v127 wieder in die automatisierte Smoke-Abdeckung.
Der Zweck ist nicht, jede Lab-App produktionsreif zu machen, sondern Core-Regressionen sichtbar zu machen:
Wenn ein App-Fenster nicht mehr erstellt, bewegt, geschlossen oder durch den WindowManager gespiegelt werden kann,
ist der USER32-/WindowManager-Vertrag wieder gebrochen.

## Neue Smoke-Gruppe

`./myos_input --smoke app_labs`

Prueft aktuell 104 App-/Lab-Asserts.

## Klassische Canaries

Die Gruppe erzeugt folgende Apps/Labs ueber ihre `wm_add_*` Pfade:

- calc
- spy
- access-lab
- pump-lab
- deadlock-lab
- section-lab
- object-lab
- wait-lab
- clip-menu-lab
- paint-lab
- drag-lab
- control-lab
- service-lab
- dialog-lab

Fuer jede App wird geprueft:

- `wm_add_*` liefert einen gueltigen, offenen Desktop-Slot.
- `Window.app_hwnd` ist ein echtes `IsWindow()`-HWND.
- `Window.app_type` stimmt.
- `GetWindowRect()` sieht die App-Geometrie.
- `MoveWindow()` verschiebt die App-Geometrie sichtbar.
- `DestroyWindow()` zerstoert das HWND und synchronisiert den WindowManager-Slot.

## ServiceLab-Reparatur

ServiceLab hatte als Top-Level noch den alten `hwnd_create()`-Pfad und damit keinen echten public USER32-WNDPROC-Lifecycle.
v128 hebt ServiceLab auf:

- `RegisterClassExA("myOS.ServiceLab")`
- `CreateWindowExA(..., "myOS.ServiceLab", ...)`
- echte `LRESULT CALLBACK servicelab_wndproc(...)`
- `WM_CLOSE` laeuft ueber `DefWindowProcA`
- `WM_DESTROY` raeumt SCM-Handles auf
- BUTTON-Kinder bleiben echte `CreateWindowExA("BUTTON", ...)`-Controls

Damit kann ServiceLab im gleichen DestroyWindow-/WindowManager-Sync-Pfad laufen wie die anderen USER32-Apps.

## AppHost-Aliase

v128 prueft ausserdem die registrierten AppHost-Aliase fuer die wichtigsten GUI- und Lab-Images.
Das ist absichtlich kein kompletter OOP-GUI-Launch-Marathon; der bestehende `apphost`-Smoke startet weiterhin ein echtes fork/exec-Console-Image (`argdump`).

## Nicht-Ziele

- Kein kompletter MDI-Pass.
- Kein vollstaendiger OOP-GUI-Integrationstest fuer alle Labs.
- Keine UI-Kosmetik.
- Kein Ersatz fuer eine spaetere Conformance-Suite.

## Naechster sinnvoller Schritt

v129 sollte MDI/child-window ownership/WM_COMMAND routing gezielt behandeln:
MDI-Client, MDI-Child, child ownership, menu command routing, move/resize limits und Destroy-Reihenfolge.
