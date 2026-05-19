myOS v49 - Process Environment API / StartupInfo retrieval

BUILD:
  myos_v49_process_environment_api

Goal:
  v48 carried STARTUPINFOA, command line and current directory into Process-Lite.
  v49 exposes that handoff through WinAPI-shaped process-environment calls, so
  apps can query their launch packet instead of only ObjectLab/WaitLab diagnostics
  reading it from Process-Lite.

New WinAPI-style calls:
  LPSTR GetCommandLineA(void);
  void  GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo);
  DWORD GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer);
  BOOL  SetCurrentDirectoryA(LPCSTR lpPathName);

Semantics in this PoC:
  - Calls are bound to the active myOS runtime/process context.
  - GetCommandLineA returns the Process-Lite command_line, falling back to image_name.
  - GetStartupInfoA returns the stored STARTUPINFOA fields: title, show-window,
    startup position and size flags.
  - GetCurrentDirectoryA follows WinAPI buffer-size behavior: if the buffer is too
    small or NULL, it returns the required size including the trailing NUL.
  - SetCurrentDirectoryA mutates the current Process-Lite current_directory.

WaitLab test:
  1) Open WaitLab.
  2) Click Spawn Child.
  3) Click EnvAPI.

Expected:
  EnvAPI enters the child runtime context and calls:
    GetCommandLineA()
    GetStartupInfoA()
    GetCurrentDirectoryA()
    SetCurrentDirectoryA("/tmp/myos-v49-child-cwd")

  Status should show:
    cmd=--from-waitlab --v49-envapi
    cwd=/tmp/myos-v49
    setCwd=TRUE
    now=/tmp/myos-v49-child-cwd
    title=WaitLab child via STARTUPINFO
    show=5
    pos=77,88

Why this matters:
  This is another step toward a real loader boundary.  CreateProcess/ShellExecute
  now carry startup metadata into Process-Lite, and WinAPI-style apps can read it
  back from their active process context.

Visible markers:
  top-left desktop marker:
    BUILD v49: process env API

  console:
    myos - v49: process environment API
    BUILD: myos_v49_process_environment_api - public STARTUPINFO/environment API over Process-Lite
