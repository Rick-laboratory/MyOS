myOS v51 - Process Module Image API

BUILD:
  myos_v51_process_module_api

Purpose:
  v51 adds the next Loader-Lite building block after STARTUPINFO, command-line, CWD, and Environment Block:
  every Process-Lite now has a main module token and image filename metadata.

New public WinAPI-shaped APIs:
  HMODULE GetModuleHandleA(LPCSTR lpModuleName);
  DWORD   GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);

Semantics implemented:
  GetModuleHandleA(NULL) returns the active runtime process main module.
  GetModuleHandleA("name") matches the current process image name/module basename, with .exe tolerance.
  GetModuleFileNameA(NULL, ...) returns the active runtime process image path.
  GetModuleFileNameA(hModule, ...) resolves the Process-Lite main module token.

Process-Lite metadata now includes:
  main_module
  image_path
  module_name

Current image path rule:
  If the image has a path/drive marker, keep it.
  Otherwise synthesize C:\myOS\System32\<image>.exe.

WaitLab test:
  1. Open WaitLab.
  2. Click Spawn Child.
  3. Click Module.

Expected status sample:
  ModuleAPI: PID=... HMOD=0x52000... named=0x52000... bad=0x0 len=...
             file=C:\myOS\System32\waitlab-child-lite.exe infoMod=0x52000...

This is intentionally not a full DLL loader yet.  It is the correct precursor:
  process image identity -> main module handle -> module filename APIs -> later LoadLibrary/GetProcAddress/DLL table.
