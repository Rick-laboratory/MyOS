myOS v169 - per-HWND OOP GDI command streams

BUILD: myos_v169_per_hwnd_gdi_streams

Goal
----
Move the OOP child GDI IPC path away from the v168 per-process command buffer
failure mode.  Win32 paints windows/DCs, not processes.  Multiple top-level
HWNDs owned by the same child process must therefore retain independent visible
content.

What changed
------------
1. MyGdiIpcCommand now carries a target HWND and stream sequence.
2. myos_apphost_child keeps the existing compact shared gdi_commands[] array,
   but treats it as a retained set of per-HWND command streams.
3. child_gdi_reset_for_hwnd() removes only commands for the target HWND and
   appends the newly painted commands for that HWND.
4. The parent compositor filters OOP GDI commands by Window.app_hwnd before
   replaying them.
5. OOP DialogLab now paints modal/modeless dialog roots into their own HWND
   stream instead of reusing the owner window's process-wide stream.
6. AppHost smoke gained a v169 regression tripwire:
   root DialogLab HWND and modeless Dialog HWND must both have nonzero tagged
   GDI commands in the same child process.

Why this matters
----------------
This intentionally breaks the old assumption that one child process has one
visible GDI buffer.  That assumption was convenient for early OOP rendering,
but it is not compatible with Win32/MSDN semantics.  The compositor can now
render multiple top-level windows from the same OOP process without replaying
whatever that process painted most recently.

Verification
------------
make clean && make
./myos_input --smoke apphost
./myos_input --smoke all

Expected v169 smoke marker:
  OOP DialogLab GDI streams are per-HWND
