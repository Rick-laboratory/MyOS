myOS v120 SDK Header Bridge
===========================

Goal
----
Expose MSDN/WinSDK-style public header names without breaking the existing codebase.

New public entry points:

    #include <windows.h>
    #include <windef.h>
    #include <winnt.h>
    #include <winerror.h>
    #include <winbase.h>
    #include <processthreadsapi.h>
    #include <handleapi.h>
    #include <synchapi.h>
    #include <memoryapi.h>
    #include <errhandlingapi.h>
    #include <libloaderapi.h>
    #include <winuser.h>
    #include <wingdi.h>
    #include <commdlg.h>
    #include <commctrl.h>
    #include <winsvc.h>

Migration rule
--------------
Existing my*.h headers are not deleted yet. They are the compatibility backend.
New or external test apps should prefer the SDK names above.

Hard gate
---------
The runtime behaviour must not change in this iteration. Build and smoke output must stay green.
