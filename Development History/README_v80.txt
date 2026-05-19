myOS v80 - SetWindowPos / WINDOWPOS Contract
=============================================

BUILD: myos_v80_1_spy_nonclient_signal_fix

Basis: v79 nonclient resize + v77.3 client-click fix.

Ziel
----
Move/Resize/Show/Minimize/Restore laufen jetzt durch einen Win32-aehnlichen
Window-Position-Contract statt direkt die Window-Struktur zu mutieren.

Neu / geaendert
---------------
1. Neue/erweiterte Win32-kompatible Konstanten und Structs:
   - WM_MOVE
   - WM_GETMINMAXINFO
   - WM_WINDOWPOSCHANGING
   - WM_WINDOWPOSCHANGED
   - WINDOWPOS
   - MINMAXINFO
   - SIZE_RESTORED / SIZE_MINIMIZED / SIZE_MAXIMIZED
   - SWP_NOREDRAW / SWP_SHOWWINDOW / SWP_HIDEWINDOW / SWP_FRAMECHANGED / SWP_NOCOPYBITS
   - HWND_BOTTOM / HWND_TOPMOST / HWND_NOTOPMOST

2. SetWindowPos ist jetzt der zentrale Positionspfad:
   - berechnet WINDOWPOS
   - holt MINMAXINFO/Minimum-Track-Size
   - sendet WM_WINDOWPOSCHANGING
   - committed x/y/cx/cy/minimized
   - sendet WM_WINDOWPOSCHANGED
   - sendet WM_MOVE und WM_SIZE mit MSDN-artigen Parametern

3. MoveWindow ist jetzt Wrapper ueber SetWindowPos.

4. ShowWindow nutzt den gemeinsamen Pfad:
   - SW_HIDE / SW_MINIMIZE -> SWP_HIDEWINDOW
   - SW_SHOW / SW_RESTORE -> SWP_SHOWWINDOW

5. v79 Nonclient Move/Resize ruft jetzt wm_set_window_pos_ex(), statt w->x/y/w/h direkt zu schreiben.

6. Version/Debug-Anzeige:
   - alle aktiven Buildmarker auf v80 gesetzt
   - oberer Rand nutzt zwei Zeilen und kuerzere Texte, damit es auf kleineren Framebuffern nicht ueberlaeuft

Testprozedur
------------
Start:

  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

A) Regression
   1. Calc oeffnen.
   2. Calc-Buttons klicken -> muessen weiter reagieren.
   3. Editor oeffnen, in Client klicken und tippen -> muss weiter gehen.
   4. START Button / Desktop-Rechtsklick testen -> muss weiter gehen.

B) v80 WindowPos Contract
   1. Calc/Editor verschieben.
      Erwartet: Bewegung klappt wie v79, aber intern ueber SetWindowPos/WINDOWPOS.

   2. Fenster an rechten/linken/oberen/unteren Kanten und Ecken resizen.
      Erwartet: alle v79-Resize-Faelle funktionieren weiter.

   3. HWND StateProbe oeffnen und subscriben.
      Erwartet: bei Move/Resize steigen state seq/dirty Meldungen.
      lastMessage sollte WM_WINDOWPOSCHANGED, WM_MOVE oder WM_SIZE sehen koennen.

   4. Minimize testen.
      Erwartet: Pfad SC_MINIMIZE -> ShowWindow/SWP_HIDEWINDOW -> WM_SIZE(SIZE_MINIMIZED).

   5. Restore/Show via Start/Taskbar falls vorhanden.
      Erwartet: SWP_SHOWWINDOW/SIZE_RESTORED-Pfad.

Hinweise / Limits
-----------------
- WM_WINDOWPOSCHANGING/CHANGED werden fuer OOP-Children als Diagnose/Notify
  geliefert; die Shared HWND State Section bleibt fuer OOP die Quelle der Wahrheit.
- Z-Order-Flags sind als API-Konstanten vorhanden, aber die echte Z-Order-Politik
  ist noch nicht vollstaendig Windows-kompatibel. Focus-last drawing bleibt aktuell
  weiterhin der wichtigste sichtbare Pfad.
- Maximizing ist vorbereitet, aber noch nicht als kompletter SW_MAXIMIZE Contract umgesetzt.
