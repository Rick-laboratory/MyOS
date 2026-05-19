myOS v55 - Console Subsystem + main(argc, argv) Entry Contract

Build marker:
  myos_v55_console_subsystem_main

v55 extends the v54 loader descriptor model with a second subsystem path:

  windows subsystem:
    imports -> WinMain(hInstance, hPrevInstance, lpCmdLine, nCmdShow)
    App creates HWND and desktop frame owns hProcess/hThread.

  console subsystem:
    imports -> command line parser -> main(argc, argv)
    return value becomes Process-Lite exit code.
    PROCESS/THREAD objects become waitable/signaled without a desktop window.

New public WinAPI-shaped export:
  ExitProcess(UINT uExitCode)

New Process-Lite diagnostics:
  subsystem
  argc
  argv_preview
  console_exit_code

Registered console test images:
  argdump
  argdump.exe
  argv-lab

WaitLab test:
  Open WaitLab and click Console.

Expected status pattern:
  ConsoleAPI: argdump main(argc=4) wait=WAIT_OBJECT_0 exit=55 pid=<pid> sub=console argv=0='argdump';1='alpha';2='two words';3='/flag'

This is still not a real Linux fork/exec console process. It is the loader contract layer that lets us keep Windows-style GUI/console subsystem behavior while the Process-Lite/Loader path is still hosted in one binary.
