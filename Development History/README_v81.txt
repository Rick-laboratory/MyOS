myOS v81 - Legacy Apps WM_COMMAND/MSDN Port Pass 1
===================================================

BUILD: myos_v81_legacy_apps_wm_command_port

Ziel
----
Nach v77-v80 läuft Shell/Frame/WindowPos deutlich MSDN-näher. v81 ist die erste
Portierungsrunde für ältere Labs, die noch aus der Prä-Win32-Phase stammen und
Toolbar-Button-Aktionen direkt im WM_LBUTTONDOWN-Pfad ausgeführt oder interne
WM_USER-Messages direkt als Button-Aktion gepostet haben.

Win32-konformer Zielpfad
------------------------
Mausklick im Client:
  WM_LBUTTONDOWN -> HitTest nur für Control/Button-Fläche -> WM_COMMAND
  WM_COMMAND     -> App-spezifischer Command-Dispatcher
  optional       -> interne Worker-/WM_USER-Message für längere Arbeit

Damit sind Button-/Menü-/Accelerator-Aktionen strukturell auf denselben Pfad
vorbereitet. Das passt zu v76 Startmenü-Command-Routing und verhindert, dass
ältere Labs wieder eigene Parallel-Click-Pfade etablieren.

Portiert in v81
---------------
- AccessLab
  Probe / Subscribe / Control gehen jetzt über WM_COMMAND.

- ObjectLab classic
  Refresh / Create Section / Close Section gehen jetzt über WM_COMMAND.

- SectionLab classic
  Create Map / Write / Read / Unmap gehen jetzt über WM_COMMAND.

- PumpLab classic
  Post Self / Stress 1000 / Hang 2s / Timer gehen jetzt über WM_COMMAND.
  Interne PMPLAB_* Messages bleiben als Worker-/Queue-Test erhalten.

- DeadlockLab classic
  SendMessageTimeout Tests gehen jetzt über WM_COMMAND.
  DLMSG_* bleibt als Legacy-kompatibler interner Message-Pfad erhalten.

- WaitLab classic
  Alle Toolbar-Buttons erzeugen jetzt erst WM_COMMAND. Der Command-Handler postet
  danach die bestehende WAITLAB_* Message, damit die lange Sync/Loader/Object-
  Logik unverändert testbar bleibt.

- SharedBus classic
  Producer-/Consumer-Buttons gehen jetzt über WM_COMMAND. BUSLAB_NOTIFY bleibt
  echtes Dirty-Signal.

Nicht absichtlich geändert
--------------------------
- OOP child apps in myos_apphost_child.c: viele laufen schon über WM_COMMAND oder
  bewusst über lokale Lab-HitTests. Das ist eine spätere Portierungsrunde.
- Nonclient/Frame/SetWindowPos Pfad aus v78-v80 bleibt unverändert.
- Spy++ v80.1 Signal-Fix bleibt drin.

Testprozedur
------------
Start:
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Regression:
  1. Terminal bewegen/resizen/minimieren/schließen.
  2. Calc öffnen, Clientbuttons klicken.
  3. Editor öffnen, in Client klicken und tippen.
  4. Spy++ öffnen, Fenster in Vordergrund holen, Fenster bewegen.
  5. AccessLab öffnen -> Probe klicken. Erwartung: kein Hänger, Status/Log ändert sich.
  6. ObjectLab classic öffnen -> Refresh/Create Section/Close Section.
  7. SectionLab classic öffnen -> Create Map/Write/Read/Unmap.
  8. PumpLab öffnen -> Post Self/Stress/Hang/Timer.
  9. DeadlockLab + PumpLab öffnen -> Fast/Slow/Hang/Cross senden.
 10. WaitLab classic öffnen -> Create/Open/Set/Reset/Wait-Buttons testen.
 11. StateBus classic öffnen -> Producer/Consumer Buttons testen.

Erwartung:
- Buttons reagieren wie vorher.
- Keine Hänger bei AccessLab/Spy++/Move/Foreground.
- Top-Badge zeigt v81 und passt zweizeilig oben auf den Bildschirm.

Nächster sinnvoller Schritt
---------------------------
v82 könnte die OOP-child-Labs in myos_apphost_child.c weiter portieren:
Toolbar-Button-HitTests -> WM_COMMAND, echte child BUTTONs wo sinnvoll, und
WM_SIZE/WM_WINDOWPOSCHANGED-Repaint statt ad-hoc-Repaint-Pfade.
