BUILD: myos_v129_mdi_child_ownership_command_routing

Ziel von v129:
MDI-Unterbau erstmals als eigener USER32-Vertrag statt als Lab-Zufall.

Neu:
- SDK ergaenzt MDI-Konstanten und Strukturen:
  CLIENTCREATESTRUCT
  MDICREATESTRUCTA
  WM_MDICREATE / WM_MDIDESTROY / WM_MDIACTIVATE / WM_MDINEXT / WM_MDIGETACTIVE / WM_MDISETMENU
  DefFrameProcA
  DefMDIChildProcA
- Builtin class "MDICLIENT" wird registriert.
- MDICLIENT besitzt Active-Child-State, idFirstChild-Sequenz und Window-Menu-State.
- WM_MDICREATE erstellt echte USER32 child HWNDs unter dem MDICLIENT.
- WM_MDIDESTROY zerstoert MDI children ueber den normalen DestroyWindow-Pfad.
- WM_MDIACTIVATE / WM_CHILDACTIVATE / WM_MDIGETACTIVE / WM_MDINEXT sind smoke-gated.
- DefFrameProcA routet WM_COMMAND/WM_SYSCOMMAND an das aktive MDI Child, wenn dieses den Command behandelt.
- MDI Child Geometry bleibt ueber GetWindowRect/MoveWindow sichtbar und stabil.
- DestroyWindow(MDICLIENT) zerstoert verbleibende Children zuerst.

Smoke:
  make clean && make -j2
  ./myos_input --smoke all

Erwartet:
  BUILD: myos_v129_mdi_child_ownership_command_routing
  mdi summary :: checks=28 pass=28 fail=0 warn=0
  SMOKE RESULT: PASS (0 failures)

Bewusst noch nicht vollstaendig:
- Kein vollstaendiges Tile/Cascade/ArrangeIcons.
- Kein echtes MDI Window-Menu-Merge.
- Keine MDI-Maximize/Restore-Frame-Verschmelzung.
- Noch kein vollstaendiger MDILab-UX-Polish-Pass.

Naechster sinnvoller Schritt:
- v130: MDI polish / cascade-tile-lite / window-list/menu-merge-lite oder
- v130: Strict handle mode / raw-handle fallback abschaltbar, falls der Core wieder wichtiger ist.
