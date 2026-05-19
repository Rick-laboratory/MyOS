myOS v79 - Nonclient Resize + SizeMove Loop
==========================================

Basis: v78_nonclient_frame_hit_test + v77.3 client-click fix.

Ziel
----
Der klassische Fensterrahmen nutzt jetzt nicht nur HTCLOSE/HTMINBUTTON/
HTCAPTION, sondern auch die volleren Nonclient-Resize-Hitcodes:

  HTLEFT, HTRIGHT, HTTOP, HTBOTTOM,
  HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT

Der Ablauf bleibt Win32-artig:

  Raw mouse -> WM_NCHITTEST -> HT* -> WM_NCLBUTTONDOWN -> Size/Move loop

Neu
---
1. Neue Message-Konstanten:

   WM_ENTERSIZEMOVE = 0x0231
   WM_EXITSIZEMOVE  = 0x0232

2. Vollere DragModes:

   DRAG_RESIZE_L/R/T/B/TL/TR/BL/RB

3. Resize von links/oben verschiebt jetzt auch x/y:

   Linke Kante ziehen  -> x bewegt sich, rechte Kante bleibt verankert.
   Obere Kante ziehen  -> y bewegt sich, untere Kante bleibt verankert.
   Ecken ziehen        -> beide passenden Achsen bewegen sich.

4. App-Minimumgrößen werden beim Resize respektiert.

5. Clientbereich schließt jetzt auch den linken Resize-Rand aus.
   Dadurch wird ein Klick auf die linke Kante nicht mehr gleichzeitig als
   App-Client-Klick gepostet.

6. OOP Child-Logger kennt WM_SIZE, WM_ENTERSIZEMOVE und WM_EXITSIZEMOVE als
   Namen, damit die Runtime-Diagnose nicht nur MSG anzeigt.

Testprozedur
------------

Start:

  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

A) Regression v77.3/v78

  1. Calc öffnen oder Startup-Calc benutzen.
  2. Calc-Buttons klicken.
     Erwartet: Buttons reagieren weiter.
  3. Editor öffnen, in Clientbereich klicken, tippen.
     Erwartet: Textinput/Caret funktioniert weiter.
  4. START klicken.
     Erwartet: Startmenü öffnet weiter.
  5. Desktop-Rechtsklick.
     Erwartet: Menü öffnet weiter.
  6. Close/Minimize klicken.
     Erwartet: funktioniert weiter über WM_SYSCOMMAND.

B) v79 Resize

  1. Rechten Rand ziehen.
     Erwartet: Breite ändert sich, x bleibt gleich.

  2. Unteren Rand ziehen.
     Erwartet: Höhe ändert sich, y bleibt gleich.

  3. Rechte-untere Ecke ziehen.
     Erwartet: Breite und Höhe ändern sich.

  4. Linken Rand ziehen.
     Erwartet: x und Breite ändern sich; rechte Kante bleibt grob verankert.

  5. Obere Kante ziehen.
     Erwartet: y und Höhe ändern sich; untere Kante bleibt grob verankert.

  6. Linke-obere Ecke ziehen.
     Erwartet: x/y/w/h ändern sich passend.

  7. Rechte-obere, linke-untere, rechte-untere Ecke testen.
     Erwartet: passende Achsen ändern sich.

C) Minimumgrößen

  1. Calc sehr klein ziehen.
     Erwartet: stoppt bei CALC_MIN_W/H.

  2. Editor/WaitLab/SectionLab sehr klein ziehen.
     Erwartet: App-spezifische Minimumgrößen werden respektiert.

D) StateProbe optional

  1. HWND StateProbe öffnen.
  2. Map + Subscribe.
  3. Fenster resizen.
     Erwartet: rect/seq/msg ändern sich live. lastMessage sollte während Resize
     WM_SIZE/WM_WINDOWPOSCHANGED/WM_EXITSIZEMOVE-artige Updates zeigen.

Grenzen
-------

- Cursor-Shape ist noch nicht implementiert.
- WM_SIZING/WM_MOVING mit RECT-Payload ist noch nicht implementiert.
- echtes modal-loop Verhalten wie Windows ist noch PoC, aber die Semantik ist
  jetzt deutlich näher: enter -> continuous resize -> exit.

Build
-----

  make clean && make

Erwartet:

  BUILD: myos_v79_nonclient_resize_sizemove
  gebaut: ./myos_input
  gebaut: ./myos_apphost_child
