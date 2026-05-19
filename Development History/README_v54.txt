myOS v54 - Loader Import Resolver + WinMain Entry Contract
==========================================================

BUILD:
  myos_v54_loader_imports_entrypoint

What changed:
  v54 turns the v46-v53 AppHost/Loader-lite path into a more Windows-like
  image startup contract.

  Instead of AppHost directly calling wm_add_* through a private launcher
  callback, every registered built-in image now has a loader descriptor:

    image name / alias
    subsystem = "windows"
    WinMain-style entrypoint
    import descriptor table
    capability template

  AppHost launch flow is now:

    ShellExecute/CreateProcess/AppHost
      -> create PROCESS-lite + THREAD-lite
      -> enter child runtime context
      -> load virtual DLL imports with LoadLibraryA
      -> resolve symbols with GetProcAddress
      -> call WinMain-like entrypoint:

           int Entry(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPSTR lpCmdLine,
                     int nCmdShow)

      -> app creates its HWND through the already-existing USER32 path
      -> desktop frame receives PID/TID/hProcess/hThread metadata

New diagnostic metadata:
  MyProcessLiteInfo / MyRuntimeContextInfo now expose:

    loader_import_count
    loader_resolved_count
    loader_entry_called
    loader_error
    loader_entry
    loader_import_preview

WaitLab:
  New "Loader" button.

  Test:
    1. Open WaitLab
    2. Spawn Child
    3. Loader

  Expected:
    LoaderAPI: PID=... entry=WinMain called=1 imports=N/N err=0 dll#=...

Notes:
  This is still not a real PE/ELF loader. The loaded DLLs are virtual myOS
  DLL table entries and GetProcAddress resolves against the internal export
  registry. The important architectural win is that apps now start through an
  image descriptor + import resolution + entrypoint contract, so a future real
  AppHost binary/PE-loader can replace the internals without changing the public
  WinAPI-style surface.

Smoke test performed:
  ShellExecuteExA("calc", SEE_MASK_NOCLOSEPROCESS)
    -> PROCESS-lite created
    -> imports resolved 12/12
    -> entry WinMain called
    -> DLL table contains kernel32/user32/gdi32
    -> DestroyWindow(app HWND)
    -> hProcess becomes waitable/signaled
