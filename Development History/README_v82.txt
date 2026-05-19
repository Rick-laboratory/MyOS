myOS v82 - Legacy Apps Resize/PosChange/MSDN Port Pass 2
========================================================

BUILD: myos_v82_legacy_apps_resize_poschange_port

Ziel
----
v81 hatte die alten Labs auf den WM_COMMAND/Command-ID Pfad gebracht.
v82 portiert dieselben Apps auf den MSDN-artigen WindowPos/Resize-Vertrag:

  WM_GETMINMAXINFO
  WM_WINDOWPOSCHANGING
  WM_WINDOWPOSCHANGED
  WM_MOVE
  WM_SIZE

Zusätzlich wurde der globale WM_MOVE/WM_SIZE Payload korrigiert:

  WM_MOVE.lParam = Client-Area Upper-Left
  WM_SIZE.lParam = Client Width/Height

Der äußere Frame bleibt über WINDOWPOS/WSTS sichtbar.

Neue Hilfsschicht
-----------------
Neu ist app_msdn_resize.h mit MyAppResizeState.
Apps cachen dort nur ihren Zustand; der WindowManager bleibt Owner des echten
WindowPos-Commits. Das entspricht dem Win32-Modell: App reagiert auf Messages,
mutiert aber nicht direkt WindowManager-x/y/w/h.

Portiert / gehärtet
-------------------
- Calc
- Editor
- Spy++
- AccessLab
- ObjectLab classic
- SectionLab classic
- PumpLab classic
- DeadlockLab classic
- WaitLab classic
- SharedBus Producer/Consumer classic
- ClipMenuLab
- PaintLab
- DragLab
- ControlLab
- ServiceLab

Besondere Änderungen
--------------------
- DragLab begrenzt Drag-Box jetzt anhand der aktuellen Clientgröße, nicht mehr
  starr anhand DRAGLAB_W/H.
- PaintLab invalidiert bei WM_SIZE/WM_WINDOWPOSCHANGED neu.
- Spy++ unterscheidet eigene WM_WINDOWPOSCHANGED-Nachrichten von WSTS-Signalen.
- Top-Debug-Badge bleibt zweizeilig und kurz genug fuer kleine Framebuffer.

Testprozedur
------------
Start:

  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Regression:

1. Terminal bewegen, resizen, minimieren, schließen.
2. Calc öffnen, resizen, Zahlenbuttons klicken.
3. Editor öffnen, resizen, in Textbereich klicken, tippen.
4. Spy++ öffnen, resizen, Terminal in den Vordergrund holen, danach Terminal bewegen.
5. AccessLab öffnen, Probe klicken, danach Fenster resizen/bewegen.
6. WaitLab classic/ObjectLab/SectionLab/PumpLab/DeadlockLab öffnen und resizen.
7. DragLab öffnen, resizen, Box ziehen; Box darf nicht mehr in alte feste Grenzen laufen.
8. PaintLab öffnen, resize ziehen, Invalidate/Draw/Stress klicken.
9. ServiceLab öffnen, resize ziehen, Buttons klicken.
10. HWND StateProbe optional öffnen: rect/seq/msg sollten bei Move/Resize weiter steigen.

Erwartung
---------
- Keine Hänger bei Spy++ + Move/Resize.
- App-Client-Klicks bleiben intakt.
- Resize/Move läuft weiter über SetWindowPos/WindowPos.
- Legacy-Labs behandeln WM_SIZE/WM_MOVE/WINDOWPOS jetzt selbst wenigstens minimal.
- Build-Badge oben zeigt v82, nicht v77/v80/v81.
