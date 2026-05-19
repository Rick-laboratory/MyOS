myOS v164 - OOP Texteditor AppHost Import/Export Fix
======================================================

BUILD: myos_v164_texteditor_fix
Base:  myos_v163_patblt_rop_v1

Problem fixed
-------------
The Start-menu/Desktop Texteditor path launches the OOP editor through the
AppHost/ProcessHost GUI-IPC path. In v163 this could fail before the child
editor process was even forked:

  MyAppHostLaunchEx(editor) -> GetLastError() == ERROR_PROC_NOT_FOUND (127)

Root cause
----------
shellapi.c::g_GuiImports had been widened for the v163 GDI work and now
requested additional gdi32 imports such as:

  PatBlt
  StretchBlt
  GetStretchBltMode / SetStretchBltMode
  GetDIBits / SetDIBits
  StretchDIBits / SetDIBitsToDevice

Those APIs already existed in wingdi.c, but winbase.c::mywin_resolve_virtual_export()
still only exported BitBlt from that newer GDI family. The AppHost loader/import
check therefore aborted GUI app startup with ERROR_PROC_NOT_FOUND. The editor was
not broken in its WndProc first; it was blocked at the loader/import boundary.

Changes
-------
1. winbase.c
   - Added the missing virtual gdi32 exports to mywin_resolve_virtual_export():
     PatBlt, StretchBlt, GetStretchBltMode, SetStretchBltMode, GetDIBits,
     SetDIBits, StretchDIBits, SetDIBitsToDevice.

2. smoke.c
   - Added a v164 AppHost regression path that launches "editor" through
     MyAppHostLaunchEx().
   - Verifies that the OOP editor child publishes initial GDI/editor state.
   - Posts WM_CHAR('x') through the parent HWND/proxy path and verifies the
     editor text buffer receives it.

3. Version strings
   - Updated Makefile/main/smoke banners to myos_v164_texteditor_fix.
   - Updated stale child diagnostic prefix from v128 to v164.

Validation
----------
Executed in this package:

  make clean && make -j$(nproc)
  ./myos_input --smoke apphost
  ./myos_input --smoke all

Results:

  SMOKE_v164_apphost.log: PASS (0 failures)
  SMOKE_v164_all.log:     PASS (0 failures)

Relevant v164 regression evidence:

  [PASS] apphost   MyAppHostLaunchEx(editor)
  [PASS] apphost   OOP editor publishes initial GDI
  [PASS] apphost   OOP editor accepts WM_CHAR

