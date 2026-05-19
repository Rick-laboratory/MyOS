myOS v84 - Keyboard Focus / Translate / Accelerator Contract

BUILD: myos_v84_keyboard_focus_translate_accelerator

Ziel:
Raw Linux key events laufen jetzt sauberer in Richtung MSDN/Win32:

  Raw key
    -> Modifier state
    -> focused HWND
    -> WM_KEYDOWN / WM_KEYUP
    -> WM_SYSKEYDOWN / WM_SYSKEYUP for Alt/system keys
    -> TranslateMessage-style WM_CHAR / WM_SYSCHAR
    -> DispatchMessage / WndProc

Wichtige Änderungen:

1. Neue Messages
   - WM_SYSKEYDOWN
   - WM_SYSKEYUP
   - WM_SYSCHAR
   - WM_SETFOCUS / WM_KILLFOCUS constants vorbereitet

2. Key state
   - MYOS_KEYSTATE_ALT ergänzt
   - GetKeyState / GetAsyncKeyState Lite ergänzt
   - MyWinSetKeyDown wird vom Input-Layer gepflegt

3. Alt-Systemkeys haben Vorrang
   - Alt+F4 -> WM_SYSKEYDOWN + WM_SYSCOMMAND/SC_CLOSE
   - Alt+Space -> WM_SYSKEYDOWN + WM_SYSCOMMAND/SC_KEYMENU
   - Plain F4 bleibt Debug-Terminal-Hotkey

4. Debug F-Keys sind nachgeordnet
   - F2..F17 starten weiter Labs/Tools
   - Alt/Ctrl+F-Key wird nicht mehr von den Debug-Launchern gefressen

5. TranslateMessage-Lite
   - mywin.c TranslateMessage erzeugt WM_CHAR/WM_SYSCHAR aus WM_KEYDOWN/WM_SYSKEYDOWN
   - Parent-Inputpfad erzeugt weiterhin WM_CHAR, damit vorhandene OOP-Loops ohne TranslateMessage kompatibel bleiben

Testprozedur:

  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

A) Version
   - Oben muss v84 keyboard stehen.
   - Console-Buildstring muss myos_v84_keyboard_focus_translate_accelerator zeigen.

B) Alt/Systemkeys
   1. Calc öffnen.
   2. Alt+F4 drücken.
      Erwartet: aktives Fenster schließt, kein neues Terminal.
   3. Plain F4 drücken.
      Erwartet: Debug-Terminal öffnet weiter.
   4. Alt+Space drücken.
      Erwartet: System-Menü des aktiven Fensters öffnet.

C) Editor/Textinput
   1. Editor öffnen und in den Clientbereich klicken.
   2. a b c / Shift+A / Enter / Backspace testen.
      Erwartet: WM_CHAR-Pfad funktioniert weiter.

D) Ctrl/Accelerator-Regressions
   1. ClipMenuLab öffnen.
   2. Ctrl+C/Ctrl+V/Ctrl+N testen, sofern Fokus dort liegt.
      Erwartet: Debug-Hotkeys fressen Ctrl-Kombos nicht.

E) Regression
   - START Button
   - Desktop-Rechtsklick
   - Calc Clientbuttons
   - Fenster Move/Resize
   - Spy++ offen lassen und Alt+F4/Systemmenü testen

Bekannte Grenze:
Viele OOP-Child-Mainloops rufen noch nicht selbst TranslateMessage auf. Deshalb erzeugt der Session-Inputpfad WM_CHAR weiterhin zentral, damit Editor/Terminal/Child-Apps kompatibel bleiben. Ein späterer Schritt kann die Child-Loops klassisch auf GetMessage -> TranslateMessage -> DispatchMessage portieren.
