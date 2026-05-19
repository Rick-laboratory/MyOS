myOS v47 - ShellExecuteA/ShellExecuteExA lite
================================================

BUILD:
  myos_v47_shell_execute_lite

Theme:
  v46 introduced an internal AppHost/Loader-lite path. v47 keeps that internal
  and adds a public WinAPI/Shell32-style launch surface:

      ShellExecuteA(...)
      ShellExecuteExA(...)

This means normal app-facing code no longer needs to know about MyAppHostLaunch().
MyAppHostLaunch() remains the shell/loader glue, while ShellExecuteA/Ex are the
public compatibility-facing calls.

New public API surface
----------------------

mywin.h now exposes:

  HINSTANCE ShellExecuteA(
      HWND hwnd,
      LPCSTR lpOperation,
      LPCSTR lpFile,
      LPCSTR lpParameters,
      LPCSTR lpDirectory,
      int nShowCmd);

  BOOL ShellExecuteExA(LPSHELLEXECUTEINFOA lpExecInfo);

Minimal SHELLEXECUTEINFOA support:

  cbSize
  fMask
  hwnd
  lpVerb
  lpFile
  lpParameters
  lpDirectory
  nShow
  hInstApp
  hProcess

Implemented flag:

  SEE_MASK_NOCLOSEPROCESS

Implemented result constants:

  SE_ERR_FNF
  SE_ERR_ACCESSDENIED
  SE_ERR_NOASSOC
  MYOS_SHELLEXECUTE_SUCCESS (> 32)

Supported verbs / associations
------------------------------

Supported verb:

  open / NULL

Registered image aliases:

  calc
  calc.exe
  calculator
  calculator.exe
  editor
  editor.exe
  notepad
  notepad.exe
  texteditor

File associations currently routed to editor:

  .txt
  .log
  .md
  .c
  .h
  .ini
  .cfg

Unknown file types return SE_ERR_NOASSOC.

Ownership rule
--------------

AppHost-created desktop frames own their hProcess/hThread handles so the shell
can terminate/close the process-lite object when the visible top-level window is
closed.

ShellExecuteA only returns success/failure via HINSTANCE and does not expose a
process handle.

ShellExecuteExA + SEE_MASK_NOCLOSEPROCESS returns a duplicated process handle to
the caller. The frame-owned original handle remains valid for window close and
lifecycle cleanup.

Internal binding
----------------

New internal binding:

  BOOL MyAppHostBindShell(WindowManager* wm);

wm_init() binds the current desktop WindowManager as the target shell desktop for
ShellExecuteA/Ex. This is intentionally internal; apps should not call
MyAppHostLaunch().

Visible marker
--------------

Top-left badge:

  BUILD v47: ShellExecute lite

Console:

  myos - v47: ShellExecuteA/ShellExecuteExA lite
  BUILD: myos_v47_shell_execute_lite - public ShellExecute over internal AppHost

Smoke tests performed
---------------------

1) ShellExecuteA("calc")

  - returns > 32
  - creates a calculator desktop frame
  - frame has process_id/thread_id/process_handle metadata
  - GetExitCodeProcess(frame hProcess) == STILL_ACTIVE

2) ShellExecuteExA(open, "/tmp/myos_v47_test.txt", SEE_MASK_NOCLOSEPROCESS)

  - routes .txt to editor
  - returns hInstApp > 32
  - returns a waitable duplicated hProcess
  - WaitForSingleObject(hProcess, 0) == WAIT_TIMEOUT while editor is live
  - caller can CloseHandle(hProcess)
  - frame-owned process handle remains valid after caller closes the duplicate

3) Close lifecycle

  - ShellExecuteExA("calc", SEE_MASK_NOCLOSEPROCESS)
  - DestroyWindow(top HWND)
  - desktop slot closes
  - process-lite object becomes signaled
  - WaitForSingleObject(caller hProcess, 0) == WAIT_OBJECT_0
  - GetExitCodeProcess(...) == 0

Strategic note
--------------

No WinAPI strategy break:

  public:    ShellExecuteA / ShellExecuteExA / CreateProcessA / CreateWindowExA
  internal:  MyAppHostLaunch / MyWinEnterProcessContext / WindowManager wiring

v47 moves the app launch path one layer closer to Windows-style public API while
keeping myOS-specific loader mechanics hidden behind the shell layer.
