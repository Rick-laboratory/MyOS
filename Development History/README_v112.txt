myOS v117 - Legacy Purge / Source-Clean Pass
============================================

Goal
----
No runtime feature changes. This version removes dead compatibility ballast after
the MSDN pendant split and ships a cleaner source tree.

Removed from active source tree
-------------------------------
- mywin.c      (empty legacy translation unit)
- myservice.c  (empty legacy translation unit)
- apphost.c    (empty legacy translation unit)

The public/API implementation now lives in the MSDN-named pendant files:
- windows.c / sdk/include/windows.h
- winuser.c / sdk/include/winuser.h
- winbase.c / sdk/include/winbase.h
- wingdi.c  / sdk/include/wingdi.h
- commdlg.c / sdk/include/commdlg.h
- winsvc.c  / sdk/include/winsvc.h
- shellapi.c / sdk/include/shellapi.h

Packaging cleanup
-----------------
- Historical README_v*.txt files moved to docs/history/.
- Compiled objects (*.o), myos_input and myos_apphost_child are not shipped.
- Old BUILD_v*.log and SMOKE_v*.log files are not shipped.

Still intentionally kept
------------------------
- mywin.h, mytypes.h, myservice.h, apphost.h:
  compatibility/internal headers still referenced by apps/runtime.
- mywin_pendant_internal.h:
  internal bridge between pendant API files during the split.

Validation
----------
Expected commands:

    make clean && make
    ./myos_input --smoke all

Expected result:

    BUILD: PASS
    SMOKE RESULT: PASS (0 failures)

Known compliance TODOs unchanged from v112
------------------------------------------
- CloseHandle(NULL) LastError edge path
- WaitForSingleObject(NULL) LastError edge path
- GetWindowRect for standalone HWNDs without WindowManager-backed state
- MoveWindow/SetWindowPos for standalone HWNDs without WindowManager-backed state
