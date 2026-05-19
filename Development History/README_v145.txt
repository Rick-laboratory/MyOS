BUILD: myos_v145_input_thread_runtime_mdi_drag_fix

Basis: v144_mdi_first_launch_drag_route_fallback

Problem, manuell reproduziert:
- Fresh boot -> MDILab #1 öffnen -> MDI child drag funktioniert nicht.
- Einmal Escape im MDILab schließt ein Child.
- Danach funktioniert Drag sofort.
- MDILab komplett schließen und neu öffnen: Drag funktioniert weiterhin sofort.

Diagnose:
- Das war kein MDILab-local State und kein weiterer Cascade/Z-Order-Fallback.
- USER32 runtime/capability context ist thread-local.
- Der evdev Input-Thread startet frisch ohne MyWinBindRuntime().
- Der erste rohe MDI-Caption-Hit-Test läuft dadurch ohne CAP_WINDOW_READ/CAP_WINDOW_CONTROL.
- GetParent()/GetWindowRect()/GetWindow()/GetClassNameA können im Input-Thread scheitern.
- Escape ging durch einen Keyboard/MDI-Hilfspfad, der zufällig im Input-Thread Runtime-Kontext bindet.
- Dadurch wurde der Input-Thread "geprimed" und spätere Drags liefen.

Fix:
- Neuer interner Guard wm_ensure_session_input_runtime() in window.c.
- Raw mouse down/up, raw mouse move und client endpoint routing bootstrappen bei fehlendem TLS-Kontext den Session/Shell-Broker.
- input_thread() bindet zusätzlich beim Start explizit eine session-input Capability.
- Keine Änderung am funktionierenden Menüpfad, BUTTON-Dedupe, v131 Access-Control oder MDI-Injection-Schutz.

Neue Smoke-Abdeckung:
- MDILab first input-thread caption drag:
  Ein frischer pthread ohne MyWinBindRuntime() führt raw down/move/up aus.
  Erwartung: session broker wird automatisch gebunden und Child bewegt sich vor Escape.

Build/Test:
make clean && make -j2
./myos_input --smoke all

Ergebnis:
BUILD: myos_v145_input_thread_runtime_mdi_drag_fix
SMOKE RESULT: PASS (0 failures)

Wichtige relevante PASS-Zeilen:
[PASS] app_labs  MDILab first input-thread caption drag
[PASS] app_labs  MDILab initial active caption drag
[PASS] app_labs  MDILab initial background caption drag
[PASS] app_labs  MDILab toolbar New one-shot
[PASS] app_labs  MDILab toolbar fast click no-pump
[PASS] app_labs  MDILab physical caption drag
