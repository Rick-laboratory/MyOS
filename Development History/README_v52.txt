myOS v52 - Process DLL Table Lite

BUILD:
  myos_v52_process_dll_table_lite

GOAL:
  v52 adds the next Loader-Lite building block after main-module identity:
  a per-process loaded-DLL table with WinAPI-shaped loader calls.

PUBLIC WINAPI-SHAPED API ADDED:
  HMODULE LoadLibraryA(LPCSTR lpLibFileName);
  BOOL    FreeLibrary(HMODULE hLibModule);
  FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName);

EXTENDED:
  GetModuleHandleA(name) now resolves both the main EXE module and loaded DLLs
  in the current Process-Lite runtime context.

  GetModuleFileNameA(hModule, ...) now resolves either the main EXE image path
  or a loaded DLL path.

MODEL:
  - HMODULE is a loader token, not a CloseHandle-owned kernel handle.
  - DLL lifetime is controlled by FreeLibrary(), not CloseHandle().
  - Each Process-Lite owns its own loaded module table.
  - LoadLibraryA("kernel32") and LoadLibraryA("kernel32.dll") resolve to the
    same per-process module entry and increment refcount.
  - Module paths are synthesized as C:\myOS\System32\<module>.dll unless a path
    was passed.
  - GetProcAddress() is backed by a myOS virtual export registry for now.

SUPPORTED VIRTUAL EXPORT EXAMPLES:
  GetCommandLineA
  GetStartupInfoA
  GetCurrentDirectoryA / SetCurrentDirectoryA
  GetEnvironmentVariableA / SetEnvironmentVariableA / ExpandEnvironmentStringsA
  GetModuleHandleA / GetModuleFileNameA
  LoadLibraryA / FreeLibrary / GetProcAddress
  CreateEventA / OpenEventA / SetEvent / ResetEvent
  WaitForSingleObject / WaitForMultipleObjects / CloseHandle
  CreateProcessA / TerminateProcess / GetExitCodeProcess
  ShellExecuteA / ShellExecuteExA
  RegisterClassExA / CreateWindowExA / DefWindowProcA / DestroyWindow
  PostMessageA / SendMessageA / GetMessageA / PeekMessageA / DispatchMessageA

DIAGNOSTICS:
  MyProcessLiteInfo and MyRuntimeContextInfo now expose:
    dll_count
    dll_preview

WAITLAB TEST:
  1. Open WaitLab
  2. Spawn Child
  3. Click "DLL API"

  Expected status line:
    DLLAPI: PID=... k1=0x... k2=0x... got=0x...
    proc(cmd=yes evt=yes miss=no)
    dll#=1 kernel32.dll:1

SMOKE TEST DONE:
  - CreateProcessA("smoke-child")
  - Enter child runtime context
  - LoadLibraryA("kernel32.dll")
  - LoadLibraryA("kernel32") returns same HMODULE and increments refcount
  - GetModuleHandleA("KERNEL32.DLL") returns same HMODULE
  - GetProcAddress("GetCommandLineA") succeeds
  - GetProcAddress("CreateEventA") succeeds
  - GetProcAddress("MissingExport") fails
  - GetModuleFileNameA(kernel32) returns C:\myOS\System32\kernel32.dll
  - FreeLibrary(second handle) decrements refcount to 1
  - TerminateProcess(child, 52) clears child loaded DLL table

NOT YET REAL:
  This is not a PE/ELF loader and does not map external DLL binaries.
  It is the correct loader-table and symbol-resolution shape for the next step.
