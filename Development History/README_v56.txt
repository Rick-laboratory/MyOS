myOS v56 - real fork/exec console process bridge
================================================

BUILD: myos_v56_real_fork_exec_console

Focus:
- First real Linux process boundary for the console subsystem.
- GUI subsystem remains in-process AppHost/WinMain for now, because HWND/WndProc/WindowManager still need IPC before GUI fork/exec.

New behavior:
- Console images such as argdump/argv-lab are launched through fork()+execv("./myos_apphost_child", ...).
- Process-Lite stores the Linux child pid in linux_pid and marks fork_exec=1.
- WaitForSingleObject(hProcess, ...) and GetExitCodeProcess(hProcess, ...) poll waitpid(WNOHANG).
- When the Linux child exits, Process-Lite PROCESS and THREAD objects become signaled and receive the real exit code.
- TerminateProcess() kills the Linux child when the Process-Lite is fork/exec-backed.

Test:
1. make clean && make
2. sudo chvt 3
3. sudo ./myos_input /dev/input/event1 /dev/input/event2
4. Open WaitLab
5. Click Console

Expected:
ConsoleAPI: fork/exec argdump ... wait=WAIT_OBJECT_0 exit=56 ... linux=<real pid> sub=console ...

Notes:
- This is intentionally console-first. GUI fork/exec needs the next IPC bridge:
  child GUI process -> myOS runtime IPC -> parent Session/WindowManager -> HWND proxy/message queue.
