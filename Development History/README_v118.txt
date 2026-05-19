myOS v118 - Breakage Audit Comments
===================================

Scope
-----
This is the v113..v118 header cleanup chain collapsed into one tested source
state. No runtime features were intentionally changed.

Main changes
------------
- mywin.h is now a thin legacy compatibility wrapper.
- New code should include <windows.h> for the MSDN/Win32 contract.
- myOS-private runtime hooks moved behind myos_private.h.
- myOS diagnostics/inspection structs and callbacks moved behind myos_diag.h.
- myservice.h is now a thin wrapper; public SCM prototypes live in <winsvc.h>.
- Source files that previously included mywin.h now include the public SDK and
  explicit private/diagnostic headers where needed.
- sdk/include/winuser.h gained DLGTEMPLATEEX / DLGITEMTEMPLATEEX so dialog
  template parsing no longer depends on mywin.h.
- Active source package stays clean: no object files or built binaries are
  included.

Build / smoke gate
------------------

    make clean && make
    ./myos_input --smoke all

Expected result:

    BUILD: PASS
    SMOKE RESULT: PASS (0 failures)

Known MSDN-compliance TODOs unchanged
-------------------------------------
- CloseHandle(NULL) still returns FALSE but does not yet set ERROR_INVALID_HANDLE.
- WaitForSingleObject(NULL) still returns WAIT_FAILED but does not yet set ERROR_INVALID_HANDLE.
- GetWindowRect for standalone non-compositor HWNDs is still incomplete.
- MoveWindow/SetWindowPos still require a WindowManager-backed top-level slot.

Next natural step
-----------------
v118 should return to actual compliance fixes: LastError discipline, invalid
handle paths, access-mask enforcement, and the remaining USER32 geometry edge
cases.
