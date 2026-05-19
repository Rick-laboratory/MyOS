myOS v105 - SDK Header Bridge (MSDN Header Names)
=================================================

Purpose
-------
This iteration adds the first public WinSDK/MSDN-style header layer while keeping
all existing project headers alive as a compatibility backend.

No runtime behaviour was intentionally changed.

New SDK headers
---------------

    sdk/include/windows.h
    sdk/include/windef.h
    sdk/include/winnt.h
    sdk/include/winerror.h
    sdk/include/winbase.h
    sdk/include/processthreadsapi.h
    sdk/include/handleapi.h
    sdk/include/synchapi.h
    sdk/include/memoryapi.h
    sdk/include/errhandlingapi.h
    sdk/include/libloaderapi.h
    sdk/include/winuser.h
    sdk/include/wingdi.h
    sdk/include/commdlg.h
    sdk/include/commctrl.h
    sdk/include/winsvc.h

Build change
------------
The Makefile now adds:

    -Isdk/include

There is also a build-time probe:

    sdk_header_probe.c

It includes <windows.h>, <commdlg.h>, <winsvc.h>, and <commctrl.h> and references
common Win32 types/constants. If the SDK facade breaks, the normal build breaks.

Compatibility rule
------------------
The old headers remain valid for now:

    mywin.h
    mytypes.h
    myobject.h
    myservice.h

They are not deleted until the project builds and smokes cleanly with the SDK
facade in place. New app/lab code should begin using the SDK headers.

Required tests
--------------

    make clean && make
    ./myos_input --smoke all

Expected result:

    SMOKE RESULT: PASS (0 failures)

Manual TTY regression pass stays the same as v104:

    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

Manual focus:

    Start menu
    Calc first-click buttons
    DialogLab Open/Save/ChooseFont
    Dialog navigation Tab/Shift+Tab/Pfeiltasten
    Menus/submenus/Escape
    ControlLab
    WaitLab/ObjectLab/ServiceLab
    Move/close all windows

Next target
-----------
v106 should continue the header migration by moving declarations from the monolithic
mywin.h into the correct SDK header buckets, while preserving the same smoke result.
