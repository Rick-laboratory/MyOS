BUILD: myos_v132_mdi_polish_tile_cascade_menu_merge

Purpose:
- Continue the v129-v131 MDI foundation with visible/useful MDI behavior.
- Add USER32 MDI polish tripwires for WM_MDITILE, WM_MDICASCADE,
  WM_MDIICONARRANGE and WM_MDISETMENU/window-list refresh.
- Add a classic parent-side MDILab so MDI is actually reachable from the
  Start menu/AppHost alias table instead of existing only inside smoke tests.

Verified:
  make clean && make -j2
  ./myos_input --smoke all

Result:
  SMOKE RESULT: PASS (0 failures)

Main new smoke coverage:
  mdi       checks=38 pass=38 fail=0 warn=0
  app_labs  checks=112 pass=112 fail=0 warn=0

Important:
- v132 is still MDI-lite, not the entire Windows MDI subsystem.
- It does not yet implement iconic child rendering, full maximize/restore frame
  merge, full MDI scrollbars, or complete Window menu merge semantics.
- It does add the first real layout/window-list contract and a visible MDILab
  canary for manual testing.
