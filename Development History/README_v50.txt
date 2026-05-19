myOS v50 - Process Environment Block API

BUILD:
  myos_v50_process_environment_block

Why this version exists:
  v48 carried STARTUPINFO into Process-Lite.
  v49 exposed command line / startup info / current directory.
  v50 adds the missing Windows-like per-process environment block.

Public WinAPI-shaped calls added:

  DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
  BOOL  SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue);
  DWORD ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize);

CreateProcessA now consumes lpEnvironment as a small ANSI MULTI_SZ block:

  "A=1\0B=2\0\0"

Semantics in this PoC:

  * lpEnvironment != NULL:
      Child receives the provided environment block.

  * lpEnvironment == NULL:
      Child inherits the parent Process-Lite environment.

  * root/runtime processes get defaults:
      SystemRoot=C:\myOS
      windir=C:\myOS
      PATH=C:\myOS\System32;C:\myOS
      TEMP=C:\Temp
      TMP=C:\Temp
      USERPROFILE=C:\Users\Rick

WaitLab test:

  1. Open WaitLab.
  2. Click Spawn Child.
  3. Click EnvAPI.

Expected status includes something like:

  EnvAPI: PID=... cmd=--from-waitlab --v50-envblock ...
          MYOS=v50 PATHneed=... setEnv=TRUE
          exp=C:\myOS|v50|child-ctx env#=4

Process-Lite diagnostics now include:

  environment_count
  environment_preview

This keeps the public surface Windows-shaped while the internal AppHost /
Process-Lite loader remains myOS-private.
