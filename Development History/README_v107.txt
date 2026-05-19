myOS v107 - SDK Header Split Final Pass
======================================

Goal
----
Continue the v105/v106 SDK migration without changing runtime behavior:
public Win32/MSDN-style headers are now the canonical declaration surface for
implemented public APIs, while legacy/internal headers remain in place until all
runtime modules are migrated safely.

What changed
------------
* sdk/include/shellapi.h added with ShellExecuteA/ShellExecuteExA and SEE_/SE_ERR constants.
* sdk/include/windows.h now exposes shellapi.h through the umbrella include.
* sdk/include/winuser.h filled out with the remaining public USER32 constants
  that still existed only in hwnd.h/mywin.h:
  - SC_* system commands
  - LB_* extended listbox messages
  - LBN_* notifications
  - SBS_* scrollbar styles
  - SBM_* scrollbar messages
  - CB_* extended combo messages
  - CBN_* notifications
* sdk_header_probe.c now compiles against <windows.h>, <commdlg.h>, <winsvc.h>,
  <commctrl.h> and <shellapi.h> and touches the extra constants/function types.
* Make/build labels updated to myos_v107_sdk_header_split_final.

Important compatibility decision
--------------------------------
No runtime ABI break in this pass.  mywin.h/hwnd.h/myobject.h/myservice.h stay
available for the current implementation and for apps that still need myOS
private APIs such as MyWinBindRuntime, MyEnumObjects, MyGetWindowState, etc.

This means the public SDK surface is now much more complete, but the source tree
is intentionally not forced to include only <windows.h> yet.  That migration is
safe only after the private myOS declarations are split into a dedicated
internal header.

Validation
----------
make clean && make        -> PASS
./myos_input --smoke all  -> PASS, 0 failures

Known MSDN compliance warnings preserved from v104/v106
------------------------------------------------------
* CloseHandle(NULL) returns FALSE but GetLastError is not yet ERROR_INVALID_HANDLE.
* WaitForSingleObject(NULL) returns WAIT_FAILED but GetLastError is not yet ERROR_INVALID_HANDLE.
* Standalone CreateWindowExA HWNDs still do not have full compositor WindowState,
  so GetWindowRect/MoveWindow are warning-only in smoke.

Manual test
-----------
sudo chvt 3
sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

Then verify Start menu, Calc first-click, DialogLab, Open/Save/ChooseFont,
ControlLab, WaitLab, ObjectLab, ServiceLab, menus/submenus and close behavior.
