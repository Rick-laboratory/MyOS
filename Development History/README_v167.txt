myOS v167 - OOP DialogLab modal isolation

BUILD: myos_v167_oop_dialog_modal_isolation
BASE:  myos_v166_oop_drag_capture_order_fix

Why this version exists
-----------------------
v165/v166 moved DragLab into the AppHost/OOP path and fixed the capture/drop
ordering bug.  v167 moves DialogLab onto the same architectural path: the shell
no longer treats dialog-lab as an in-process desktop app by default.  dialog-lab
now launches as a real AppHost GUI child process and the parent compositor hosts
it through the IPC proxy window.

This is deliberately architecture-compliance-first: Smoke tests were updated to
expect the isolated AppHost path instead of preserving the older in-process
APP_DIALOGLAB assumption.

Changes
-------
- shellapi.c:
  - dialog-lab/dialoglab/dialog-lab.exe registered as gui-ipc/AppHost entries.
  - dialog-lab-classic remains available as the legacy in-process WinMain path
    for DLGTEMPLATE/DialogBoxParamA parity testing while OOP DialogLab hardens.
  - DialogLab GUI IPC default size is 920x320.

- window.c:
  - Start menu DialogLab command now launches MyAppHostLaunch("dialog-lab", ...)
    instead of direct wm_add_dialoglab.

- myos_apphost_child.c:
  - added child-owned OOP DialogLab window class and WndProc.
  - creates 13 command BUTTON HWNDs from the child process through parent-brokered
    CreateWindowExA IPC.
  - keeps child-owned status/counters/rendering state and publishes GDI/status
    over the existing GUI IPC command buffer.

- main.c / dialog keyboard path:
  - added a defensive Tab fallback after IsDialogMessageA fails: Tab must be
    consumed/wrapped for dialog roots instead of falling through into default
    command/EndDialog behavior.
  - this fixes the observed "last Tab closes the dialog" failure mode.

- app_dialoglab.c:
  - AccessProbe modal/modeless OK/Cancel buttons moved from y=43 DLU to y=39 DLU
    so the first two dialog boxes no longer visually sit too low at the bottom.

- smoke.c:
  - apphost smoke now verifies dialog-lab is registered/launched through AppHost.
  - verifies OOP DialogLab publishes child-owned GDI and 13 child HWND controls.
  - verifies GetNextDlgTabItem wraps circularly across all OOP DialogLab controls.
  - shell_broker smoke now expects a dialog-lab APP_IPC_PROXY instead of the old
    in-process APP_DIALOGLAB slot.

Validation
----------
make clean && make
./myos_input --smoke apphost
./myos_input --smoke all

Observed result in this package:
- Build PASS, no compiler warnings
- apphost smoke PASS: 17 PASS, 0 FAIL, 0 WARN
- all smoke PASS: 1035 PASS, 0 FAIL, 0 WARN

Notes
-----
This is the first OOP DialogLab bridge step.  The full legacy DLGTEMPLATE /
DialogBoxParamA exercise remains available through dialog-lab-classic while the
child-side modal/dialog-manager semantics are pulled across process boundaries in
future versions.
