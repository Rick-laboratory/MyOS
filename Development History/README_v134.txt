BUILD: myos_v134_mdi_lab_render_input_fix

Purpose:
  Visual/interactive MDILab repair pass after v131/v133 HWND access hardening.

Fixed:
  - MDILab toolbar no longer sits underneath the HMENU compositor chrome.
  - MDILab uses real BUTTON child HWNDs only; duplicate hand-painted fake
    toolbar buttons were removed.
  - MDICLIENT layout now reserves the app menu bar and status line.
  - MDILab rendering enters the owning app runtime context before reading MDI
    child/client HWND geometry, so v131 HWND access control does not make
    children logically exist but visually disappear.
  - Resize path relayouts toolbar + MDICLIENT.
  - app_labs smoke now probes MDILab child controls at the post-menubar toolbar
    position instead of the old overlapped y=18 coordinate.

Verified:
  make clean && make -j2
  ./myos_input --smoke all
  SMOKE RESULT: PASS (0 failures)
