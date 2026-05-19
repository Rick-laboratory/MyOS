myOS v53 - Process Loader Errors + DLL Search Path Lite

BUILD:
  myos_v53_process_loader_errors_dllpath

What changed:
  - Adds process-local GetLastError()/SetLastError().
  - Extends Process-Lite diagnostics with last_error and dll_directory.
  - Adds SetDllDirectoryA()/GetDllDirectoryA().
  - LoadLibraryA now resolves known virtual DLLs through the process DLL directory
    before falling back to C:\myOS\System32.
  - LoadLibraryA now returns ERROR_MOD_NOT_FOUND for unknown virtual DLLs.
  - GetProcAddress returns ERROR_PROC_NOT_FOUND for missing virtual exports.
  - ShellExecuteA/ShellExecuteExA and CreateProcessA now set useful LastError values
    on the common failure paths.

Known virtual DLLs for v53:
  kernel32.dll, user32.dll, gdi32.dll, shell32.dll, advapi32.dll

WaitLab test:
  1. Open WaitLab.
  2. Click Spawn Child.
  3. Click DLL API.

Expected:
  - DLL API sets child DLL dir to C:\myOS\TestDlls.
  - LoadLibraryA("kernel32.dll") succeeds.
  - LoadLibraryA("missing53.dll") fails with ERROR_MOD_NOT_FOUND (126).
  - GetProcAddress(...,"NoSuchExport") fails with ERROR_PROC_NOT_FOUND (127).
  - Process line shows lastErr and dllDir.
