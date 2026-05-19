myOS v65 - OOP PaintLab over GUI IPC + GDI line buffer
========================================================

BUILD: myos_v65_oop_paintlab_gui_ipc

Major change
------------
PaintLab is now a real out-of-process GUI app.

paint-lab / paintlab / paint-lab.exe now run through:

  AppHost gui-ipc subsystem
   -> Process-Lite + THREAD-lite
   -> fork/exec ./myos_apphost_child --gui paint-lab
   -> child-side WinMain/WndProc/Paint state
   -> RegisterClassExA/CreateWindowExA over IPC
   -> WM_LBUTTONDOWN/WM_MOUSEMOVE/WM_LBUTTONUP/WM_PAINT over IPC
   -> child writes generic GDI commands into shared section
   -> parent WindowManager renders the GDI command buffer only

New GDI IPC command
-------------------

  MYOS_GDI_OP_LINE

The generic parent renderer now supports line commands from the child.  This
lets PaintLab draw stroke segments without parent-side PaintLab-specific UI
knowledge.

Mouse/capture behavior
----------------------

The IPC proxy parent endpoint now auto-captures the HWND on WM_LBUTTONDOWN and
releases it on WM_LBUTTONUP.  The child still uses user32-like SetCapture /
ReleaseCapture stubs for app-side semantics/diagnostics, while the parent owns
the real cross-process routing behavior.

Shared diagnostics
------------------

ProcessHost / shared section now mirrors PaintLab diagnostics:

  paint_enabled
  paint_revision
  paint_segments
  paint_mouse_down
  paint_capture_count
  paint_release_count
  paint_move_count
  paint_clear_count
  paint_last_x / paint_last_y
  paint_status

Tests performed
---------------

Build:

  make clean && make

Smoke tests:

  PaintLab:
    fork/exec GUI child
    CreateWindowExA over IPC
    initial GDI buffer
    WM_LBUTTONDOWN + WM_MOUSEMOVE + WM_LBUTTONUP
    stroke segments written as GDI line commands
    SetCapture/ReleaseCapture diagnostics
    WM_CLOSE -> exit code 65

  Editor regression:
    WM_CHAR 'H' + 'i'
    preview == "Hi"
    F10 Save
    WM_CLOSE -> exit code 64

  Calculator regression:
    Button 7
    display == "7"
    WM_CLOSE -> exit code 63
