myOS v119 - App/Lab Breakage Audit
==================================

Goal:
- No runtime feature changes.
- Add upfront breakage comments for apps/labs and OOP childhost.
- Keep v118 core audit markers.
- Keep build and smoke green.

Main additions:
- docs/APP_LAB_BREAKAGE_AUDIT_v119.md
- Inline markers:
  - AUDIT(v119-app)
  - AUDIT(v119-lab)
  - AUDIT(v119-oop)

Build/test:
  make clean && make
  ./myos_input --smoke all

Result from package build:
  BUILD: PASS
  BUILD WARNINGS: 0
  SMOKE: PASS
  SMOKE RESULT: PASS (0 failures)
