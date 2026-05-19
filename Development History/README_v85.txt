myOS v85 - Menu Keyboard Navigation / Menu Loop Contract
========================================================

BUILD: myos_v85_menu_keyboard_navigation

Was neu ist
-----------
1. Popup-/System-/Startmenues haben jetzt einen aktiven Menu-Loop-State.
2. Neue MSDN-nahe Menu-Messages:
   - WM_INITMENU
   - WM_INITMENUPOPUP
   - WM_MENUSELECT
   - WM_ENTERMENULOOP
   - WM_EXITMENULOOP
   - WM_CANCELMODE
3. Geoeffnete Menues besitzen jetzt die Tastatur zuerst:
   - Up/Down wechseln die Auswahl
   - Home/End springen an Anfang/Ende
   - Enter/Space fuehrt den ausgewaehlten Command aus
   - Esc schliesst das Menue ohne Aktion
   - Hotletters/Buchstaben waehlen einen passenden Eintrag
4. Die Auswahl wird sichtbar hervorgehoben.
5. Maus-Hover aktualisiert die Auswahl und sendet WM_MENUSELECT.
6. Debug-F-Keys greifen nicht, solange ein Menue offen ist.

Testprozedur
------------
Start:

    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2

A) Version pruefen
   - Oben muss "v85 menu keys" stehen.
   - Console-Buildstring muss myos_v85_menu_keyboard_navigation zeigen.

B) System-Menue per Alt+Space
   1. Calc oder Editor oeffnen.
   2. Alt+Space druecken.
   Erwartet: System-Menue oeffnet sich.
   3. Down/Up druecken.
   Erwartet: Highlight bewegt sich sichtbar.
   4. Esc druecken.
   Erwartet: Menue schliesst, Fenster bleibt offen.
   5. Alt+Space erneut, Down bis Minimize/Close, Enter.
   Erwartet: Command laeuft ueber WM_SYSCOMMAND.

C) System-Menue per Titelleisten-Rechtsklick
   1. Rechtsklick auf Titelleiste.
   2. Up/Down/Enter/Esc testen.
   Erwartet: Gleiches Verhalten wie Alt+Space.

D) Startmenue per Tastatur
   1. START klicken.
   2. Up/Down bewegt Highlight.
   3. Enter startet den markierten Eintrag.
   4. Esc schliesst ohne Aktion.
   5. Buchstaben/Hotletters testen, z.B. C fuer Calc/Control je nach Treffer.

E) Desktop-Kontextmenue
   1. Rechtsklick auf Desktop.
   2. Up/Down/Enter/Esc testen.
   Erwartet: Keyboard-Pfad identisch zum Startmenue.

F) Regression
   - Alt+F4 schliesst weiter aktives Fenster.
   - Plain F4 darf weiter Debug-Terminal starten, aber nicht solange ein Menue offen ist.
   - Editor tippen funktioniert nach Menue-Schliessen weiter.
   - Calc Buttons funktionieren weiter.
   - Move/Resize/Spy++/AccessLab Probe wie v82.1 testen.
