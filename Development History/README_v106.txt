myOS v106 - SDK Header Split Pass 1
==================================

Goal
----
Start moving public Win32/MSDN declarations out of the legacy mywin.h bridge and
into canonical SDK header buckets, without changing runtime behavior.

This iteration intentionally does not add runtime features. It is a safe refactor
step gated by the existing v104/v105 smoke runner.

What changed
------------
Public SDK headers now declare their own public constants, structs and function
prototypes instead of simply including mywin.h:

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
    sdk/include/windows.h

Compatibility rule
------------------
The old project headers are still retained and are still used by the current
runtime source files. They remain the backend while the public SDK surface is
split into MSDN-style header buckets step by step.

Important ABI note
------------------
MSG still carries the private myOS queue payload in the SDK header for now.
That preserves the current runtime ABI. A later compliance pass must remove the
private MSG tail from the public ABI and move that state into an internal sidecar.

Build/test
----------

    make clean && make
    ./myos_input --smoke all

Expected result:

    BUILD: PASS
    SMOKE RESULT: PASS (0 failures)

Manual TTY test remains unchanged:

    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

Manual checklist
----------------
- Start menu opens.
- Calc opens, first click on buttons works, window closes.
- DialogLab opens, Common Dialogs still open.
- Menus/submenus react and Escape closes modal menu state.
- ControlLab basic controls react.
- WaitLab/ObjectLab/ServiceLab still open and close.
