myOS v59 - GUI AppHost child: CreateWindowExA over IPC

BUILD: myos_v60_cross_process_message_queue

What changed:
  v58 gave every real fork/exec child a socketpair + shared section.
  v59 uses that bridge for the first GUI-side syscall shape:

      ShellExecuteExA("ipc-gui-lab")
        -> PROCESS/THREAD-lite
        -> ProcessHost fork/execs ./myos_apphost_child --gui ipc-gui-lab
        -> child writes a CREATE_WINDOW request into shared memory
        -> child sends MYOS_IPC_OP_CREATE_WINDOW over socketpair
        -> parent drains ProcessHost IPC
        -> parent enters the child RuntimeContext
        -> parent executes RegisterClassExA/CreateWindowExA
        -> parent attaches HWND/Desktop frame to the real child Process-Lite
        -> parent ACKs HWND/window-index back to child

New IPC opcodes:
  MYOS_IPC_OP_CREATE_WINDOW
  MYOS_IPC_OP_WINDOW_ACK
  MYOS_IPC_OP_WINDOW_FAIL

New child image:
  ipc-gui-lab
  ipcgui

New parent-side desktop app type:
  APP_IPC_PROXY

Testing:
  1. make clean && make
  2. sudo chvt 3
  3. sudo ./myos_input /dev/input/event1 /dev/input/event2
  4. Open WaitLab
  5. Click "GUI IPC"

Expected:
  - a new window titled myOS IPC GUI Child / ipc-gui-lab appears
  - WaitLab status line shows something like:

      GuiIPC: pid=<pid> linux=<linuxpid> hwnd=<hwnd> idx=<slot> req=1 ack=1 msg>=3 hb>=1 title=ipc-gui-lab

  - closing the new GUI IPC proxy window terminates the backing Linux child process
    through the existing ProcessHost/TerminateProcess bridge.

Still intentionally not done in v60:
  - child-side GetMessage/DispatchMessage loop over IPC
  - WM_* delivery back into the child process
  - real cross-process WndProc execution
  - shared render surfaces per GUI child

That is the next layer: v60 should make the parent route basic WM_CLOSE/WM_PAINT/WM_LBUTTONDOWN messages back to the child over this same channel.
