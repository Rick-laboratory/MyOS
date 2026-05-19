BUILD: myos_v135_mdi_hit_activation_bounds_fix

v135 fixes the first real manual MDILab interaction bugs found after the v132/v134 MDI work:

- nested child hit-testing now routes clicks through the MDICLIENT to the actual MDI child HWND
- clicking an MDI child activates that child through DefMDIChildProcA
- the Window menu child entries activate the selected MDI child instead of being routed as a normal WM_COMMAND to the old active child
- app-menu WM_COMMAND dispatch enters the target app owner context before calling SendMessageA, preserving the v131 HWND access-control hardening
- cascade layout keeps visible children inside the MDICLIENT bounds while myOS still has no MDI scrollbars
- MDILab visually marks the active child and caps its local visual child list to avoid invisible/untracked lab children

Verified:
  make clean && make -j2
  ./myos_input --smoke all
