myOS v46 - AppHost / Loader-Lite
=================================

BUILD:
  myos_v46_apphost_loader_lite

Goal
----
v46 moves app startup away from "just call wm_add_* directly" and toward a
Windows-like loader path:

  AppHost registered image
    -> Process-Lite PROCESS object
    -> Thread-Lite THREAD object
    -> per-image capability template
    -> MyWinEnterProcessContext(child PID)
    -> top-level HWND creation under the child identity
    -> desktop Window slot stores PID/TID/hProcess/hThread/image

This is still not Linux fork/exec isolation.  It is the next substrate layer
between v45's runtime context and a future real AppHost process / loader.

New files
---------
  apphost.h
  apphost.c

New public-ish API
------------------
  BOOL MyAppHostLaunch(WindowManager* wm,
                       LPCSTR lpImageName,
                       int x, int y,
                       LPCSTR lpTitle,
                       LPCSTR lpPath,
                       MyAppLaunchResult* lpResult);

  BOOL MyAppHostIsRegistered(LPCSTR lpImageName);

  BOOL MyWinCreateProcessWithCapability(LPCSTR lpApplicationName,
                                        LPSTR lpCommandLine,
                                        const Capability* lpChildCapability,
                                        BOOL bInheritHandles,
                                        LPPROCESS_INFORMATION lpProcessInformation);

Registered images
-----------------
  calc / calculator
  editor
  spy
  access-lab
  pump-lab
  deadlock-lab
  section-lab
  object-lab
  wait-lab
  clip-menu-lab
  paint-lab
  drag-lab
  control-lab
  service-lab

SharedBusLab is still a legacy direct pair launcher for now because it creates
two linked windows.  It should become two AppHost images in a later version.

Window metadata
---------------
Window now carries loader/process fields:

  process_id
  thread_id
  process_handle
  thread_handle
  image_name

AppHost fills these for registered-image launches.  Legacy direct wm_add_*
launches leave them zero.

Close behavior
--------------
When an AppHost-launched window is closed, the desktop slot now also terminates
its PROCESS/THREAD-lite lifetime:

  DestroyWindow(top HWND)
    -> wm_on_destroyed_hwnd()
    -> wm_mark_slot_closed()
    -> TerminateProcess(child PID, 0)
    -> close loader-side hProcess/hThread handles

This makes ObjectLab/WaitLab see Windows-like process lifetime instead of
leaking live Process-Lite records after the visible app is gone.

Integrated launch paths
-----------------------
These now use AppHostLaunch:

  startup Calculator
  F3 Calculator hotkey
  Start menu registered single-window apps
  Desktop file double-click -> editor

Terminals still use the old terminal/fork shell path and are intentionally not
part of AppHost yet.

Smoke test performed
--------------------
A separate unit-style C smoke test created a WindowManager/HWNDManager/IPCBus,
launched "calc" through AppHost, verified:

  - window slot exists
  - process_id/thread_id are attached
  - MyGetProcessLiteInfo(pid) reports STILL_ACTIVE
  - DestroyWindow(top HWND) closes the shell slot
  - process becomes MYWIN_PROCESS_EXITED
  - child handle table count becomes 0

Build command
-------------
  make clean && make

Expected visible marker
-----------------------
  top-left badge: BUILD v46: AppHost loader-lite
  console: BUILD: myos_v46_apphost_loader_lite - registered images + process-bound desktop frames
