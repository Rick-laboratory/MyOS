myOS v139 - mdi_command_dedupe_alt_drag_reset
================================================

Basis: myos_v138_mdi_physical_menu_caption_route
Build string: myos_v139_mdi_command_dedupe_alt_drag_reset

Ziel:
- MDILab New/Next/Tile/Cascade/Close dürfen nicht mehr durch BUTTON focus notifications doppelt feuern.
- Bare Alt/F10 darf keinen direkten Top-Level-Menücommand auslösen.
- Nach Close aller MDI children muss CW_USEDEFAULT/Cascade-Seed wieder am ersten Slot starten.
- MDI child caption drag muss über den echten WindowManager/raw mouse Pfad smoke-gated sein.

Wichtigste Fixes:
1. app_mdilab.c
   - WM_COMMAND unterscheidet jetzt sauber toolbar/control notifications von menu/accelerator commands.
   - Toolbar-HWNDs führen Aktionen nur bei BN_CLICKED aus.
   - BN_SETFOCUS/BN_KILLFOCUS mit derselben LOWORD(id) werden ignoriert.
   - Das behebt: ein Klick auf New erzeugt zwei children; Next überspringt jedes zweite child.

2. window.c
   - Bare Alt/F10 armt/öffnet Menü, ruft aber keinen direkten ersten Top-Level-Command auf.
   - MDI caption raw drag latch bleibt vorhanden: physischer HTCAPTION-Mousedown kann MDI child direkt im MDICLIENT verschieben.

3. winuser.c
   - Wenn der letzte MDI child zerstört ist, wird mdiChildSeq zurückgesetzt.
   - Dadurch startet New nach close-all wieder bei der ersten Cascade/CW_USEDEFAULT-Position.

4. smoke.c
   - Neuer MDILab physical/manual Contract:
     * bare Alt direct-menubar guard
     * toolbar New one-shot
     * close-all empties Window menu
     * New after close-all resets cascade
     * physical WindowManager raw caption drag

Verifikation:
    make clean && make -j2
    ./myos_input --smoke all

Ergebnis:
    BUILD: myos_v139_mdi_command_dedupe_alt_drag_reset
    SMOKE RESULT: PASS (0 failures)

Hinweis:
- Der echte sudo chvt/evdev-Handtest kann in dieser Umgebung nicht ausgeführt werden.
- Der Smoke deckt jetzt aber den WindowManager-backed raw mouse Pfad ab, nicht nur direkte SendMessage-Tests.
