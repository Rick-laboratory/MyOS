myOS v108 - MSDN pendant C layout hard pass
===========================================

Goal
----
Make the codebase easier to navigate directly from MSDN names:

    sdk/include/windows.h  -> windows.c     umbrella TU, no exported symbols yet
    sdk/include/winuser.h  -> winuser.c     USER32 + transitional old mywin surface
    sdk/include/winbase.h  -> winbase.c     next extraction target for KERNEL32
    sdk/include/wingdi.h   -> wingdi.c      next extraction target for GDI32
    sdk/include/commdlg.h  -> commdlg.c     next extraction target for COMDLG32
    sdk/include/winsvc.h   -> winsvc.c      SCM/ADVAPI32 service entrypoints
    sdk/include/shellapi.h -> shellapi.c    ShellExecute + apphost launch glue

Hard rule from v108 onward
--------------------------
Public Win32/MSDN functions should live in the matching public pendant .c file.
Private helpers may stay static directly below the function, or later move into
internal files only when a subsystem becomes too large.

What changed in v108
--------------------
* The former monolithic mywin.c implementation has moved to winuser.c.
  This keeps the current runtime ABI stable while giving the biggest USER32
  surface a real MSDN-named home.
* mywin.c is now an empty legacy compatibility translation unit.
* Service Control Manager implementation moved from myservice.c to winsvc.c.
* ShellExecute/AppHost implementation moved from apphost.c to shellapi.c.
* windows.c exists as the umbrella pendant for windows.h and intentionally
  exports no symbols yet.
* winbase.c, wingdi.c, commdlg.c stay as extraction targets. Moving those
  function groups out of winuser.c is now a mechanical follow-up pass, not a
  design question.

Why not move every single function group in this one pass?
---------------------------------------------------------
The old monolith has lots of shared file-local static state. v108 performs the
safe hard cut first: names and homes become MSDN-shaped without changing runtime
behavior. The next passes can extract KERNEL32/GDI32/COMDLG32 groups from
winuser.c one group at a time, with ./myos_input --smoke all after each move.

Tests
-----
    make clean && make
    ./myos_input --smoke all

Expected:
    BUILD: PASS
    SMOKE RESULT: PASS (0 failures)
