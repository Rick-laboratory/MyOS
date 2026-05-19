myOS v58 - per-child IPC channel: socketpair + shared section

BUILD: myos_v58_child_ipc_bridge

What changed:
  v56 introduced real fork/exec console children.
  v57 centralized Linux lifetime tracking in ProcessHost.
  v58 gives every real child process a communication bridge:

    parent ProcessHost entry
      -> socketpair(AF_UNIX, SOCK_DGRAM) control channel
      -> POSIX shared memory section /myos_v58_<pid>_<tick>
      -> child receives --ipc-fd, --shared-name, --my-pid before --console
      -> exec child sends HELLO/PING/EXIT messages
      -> child writes heartbeat/status/argv/exit to shared section
      -> ProcessHost drains IPC during Poll/GetInfo/Wait

New internal header:
  process_ipc.h

ProcessHost diagnostics now include:
  ipc_enabled
  ipc_messages
  ipc_hello
  ipc_exit_report
  ipc_last_opcode / ipc_last_value / ipc_last_text
  shared_name
  shared_heartbeat
  shared_child_pid
  shared_argc
  shared_exit_code
  shared_status
  shared_argv_preview

WaitLab test:
  1. Open WaitLab
  2. Click Console
  3. Expected status:
       ConsoleAPI: IPC ... exit=58 ... msg>=3 hello=1 x=1 hb>=2 sh=argdump-exiting ...

Why this matters:
  This is the exact bridge needed before GUI fork/exec. Later CreateWindowExA,
  PostMessage/GetMessage, HWND proxy commands and shared render surfaces can ride
  over this ProcessHost-owned child channel instead of direct in-process pointers.

Still intentionally not done in v58:
  - GUI apps are not fork/exec yet.
  - HWND/WndProc dispatch is not remote yet.
  - The shared section is diagnostic/control-plane payload for now, not a DWM surface.
