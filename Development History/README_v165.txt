myOS v165 - Process Isolation Step: DragLab moved to AppHost/OOP
=================================================================

BUILD: myos_v165_process_isolation
Base:  myos_v164_texteditor_fix

Goal
----
This version continues the architecture-first process-isolation push.  Smoke
labs remain test tools; the architecture target is that normal app launches go
through a real process/AppHost boundary instead of depending on parent-process
WinMain islands.

What changed
------------
1. DragLab is now registered as a normal OOP/AppHost GUI image.
   - shellapi.c maps drag-lab / draglab / drag-lab.exe to subsystem "gui-ipc".
   - The old parent-process app_winmain_draglab remains only as a classic helper,
     not as the normal Shell/AppHost launch path.

2. myos_apphost_child gained a child-owned DragLab WinMain/WndProc.
   - class: myOS.DragLab.OOP
   - state lives in the Linux child process:
     box position, dragging flag, capture/release counters, move/drop/cancel
     counters and log/status text.
   - rendering is published via the existing generic cross-process GDI command
     buffer, so the parent compositor stays a renderer/interpreter rather than
     owning the app logic.

3. Hotkey paths were moved further away from direct wm_add launch islands.
   - F12 now launches ObjectLab through AppHost.
   - F13 now launches WaitLab through AppHost.
   - F15 now launches DragLab through AppHost/OOP.
   - F16 now launches ControlLab through AppHost/OOP.

4. AppHost smoke coverage was extended.
   - Verifies drag-lab is registered.
   - Launches drag-lab through MyAppHostLaunchEx.
   - Confirms ProcessHost/GDI publication from the child.
   - Posts WM_LBUTTONDOWN/WM_MOUSEMOVE/WM_LBUTTONUP and confirms the child-owned
     drag/capture counters changed through ProcessHost diagnostics.

Validation
----------
make clean && make
  PASS, no compiler warnings observed.

./myos_input --smoke apphost
  PASS: checks=12 pass=12 fail=0 warn=0

./myos_input --smoke all
  PASS: 1030 PASS lines, 0 FAIL lines, 0 WARN lines
  Final line: SMOKE RESULT: PASS (0 failures)

Architectural note
------------------
This does not claim full process isolation for every remaining app yet.  It moves
one more meaningful WinMain island (DragLab) over the real fork/exec + AppHost +
IPC + GDI-buffer path and proves that input/capture behavior is child-owned.  The
remaining obvious in-process launch islands after v165 are primarily Spy,
AccessLab, PumpLab, DeadlockLab, ServiceLab, DialogLab and MDILab/classic helper
paths.
