BUILD: myos_v126_menus_accelerators_modal_loop_contract

Ziel von v126:
- Menus / Accelerators / Modal-Loop-Vertrag härten.
- Keine App-/Lab-Kosmetik.
- TrackPopupMenu nicht mehr nur als "ersten Command nehmen" behandeln, sondern als deterministischen USER32-lite Modal-Loop mit Notifications und Restore-Vertrag.

Geändert:
- TrackPopupMenu:
  - sendet WM_ENTERMENULOOP / WM_EXITMENULOOP
  - sendet WM_INITMENU / WM_INITMENUPOPUP
  - sendet WM_MENUSELECT für Auswahl und Close
  - sendet WM_UNINITMENUPOPUP beim Ende
  - TPM_RETURNCMD dispatcht keinen WM_COMMAND
  - non-TPM_RETURNCMD dispatcht genau einen WM_COMMAND an den Owner
  - disabled-only Menüs canceln ohne WM_COMMAND
  - setzt ERROR_CANCELLED bei deterministischem Cancel
  - bewahrt/restored Capture und Focus um den Lite-Modal-Loop herum

- Owner-draw Menüpfad:
  - ausgewählte MF_OWNERDRAW Items senden WM_MEASUREITEM und WM_DRAWITEM als Smoke-Tripwire.

- Accelerators:
  - FSHIFT und FALT im SDK ergänzt
  - TranslateAcceleratorA matcht Ctrl/Shift/Alt deterministisch
  - WM_SYSKEYDOWN wird akzeptiert
  - systemische Commands wie SC_CLOSE dispatchen als WM_SYSCOMMAND
  - kein Modifier-Match bedeutet kein Dispatch

- winerror.h:
  - ERROR_CANCELLED = 1223 ergänzt

Smoke:
- menu summary jetzt 44 checks, 44 pass, 0 fail, 0 warn
- smoke all bleibt grün

Geprüft:
  make clean && make -j2
  ./myos_input --smoke all

Ergebnis:
  BUILD: myos_v126_menus_accelerators_modal_loop_contract
  SMOKE RESULT: PASS (0 failures)

Bewusst noch nicht gelöst:
- kein echter gerenderter/interaktiver Popup-Tracker
- keine echte Hover-/Mouse-navigation über Popup-Levels
- keine vollständige Windows-Menü-HitTest/Layout-Engine
- keine echte systemweite Menu-Owner-Lifetime wie USER32 in Windows

v126 ist also kein kompletter Menü-Stack. Es ist der Vertragsschutz dafür, dass folgende Refactors nicht wieder Focus/Capture/Queue/Menu-Dispatch zerlegen.
