BUILD: myos_v128_apps_labs_repair_contract

Ziel von v128:
Apps/Labs nach den USER32/KERNEL32-Vertragsarbeiten aus v122-v127 wieder als brauchbare Canaries absichern.
Keine Feature-Orgie und keine kosmetische Lab-Stabilisierung: die Labs werden gegen den stabileren Vertrag gestartet,
bewegt, geschlossen und als AppHost-Aliase geprüft.

Wichtigste Änderung:
- neue Smoke-Gruppe: app_labs
- 104 PASS-Checks fuer klassische App-/Lab-Canaries
- ServiceLab wurde vom alten hwnd_create-Top-Level auf eine echte CreateWindowExA/WNDPROC-Top-Level-Klasse gehoben.
  Die BUTTON-Child-HWNDs bleiben echte USER32-Kinder und sind jetzt ueber ChildWindowFromPoint/DestroyWindow mitgeprueft.

Geprueft:
  make clean && make -j2
  ./myos_input --smoke all

Erwartetes Ergebnis:
  BUILD: myos_v128_apps_labs_repair_contract
  SMOKE RESULT: PASS (0 failures)

Neue App/Lab-Tripwires:
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
- editor

Pro App/Lab wird geprueft:
- wm_add_* liefert einen sichtbaren WindowManager-Slot
- app_hwnd ist ein echtes live HWND
- AppType stimmt
- GetWindowRect funktioniert
- MoveWindow funktioniert und ist sichtbar
- DestroyWindow zerlegt HWND und synchronisiert den WindowManager-Slot
- ServiceLab hat echte BUTTON-Child-HWNDs unter dem USER32-Parent

AppHost-Alias-Tripwires:
- calc
- editor
- clip-menu-lab
- paint-lab
- control-lab
- dialog-lab
- service-lab
- drag-lab
- access-lab
- wait-lab
- object-lab
- section-lab
- argdump

Bewusst nicht geloest:
- MDI ist noch kein eigener Contract-Pass.
- OOP GUI-Labs werden hier nicht alle live gestartet, damit --smoke all schnell und deterministisch bleibt.
- Apps/Labs sind weiterhin Canaries, nicht Compliance-Suite.
