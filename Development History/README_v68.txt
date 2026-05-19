myOS v68 - MSDN Mouse Parameters + Child Kernel Syscall Bridge
==============================================================

Build checked with:
  make clean && make

Resulting binaries:
  ./myos_input
  ./myos_apphost_child

Run pattern stays the same, for example:
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Main v68 changes
----------------

1) MSDN-compliant mouse message parameters

The central desktop/input dispatch no longer sends mouse coordinates as:
  wParam = x
  lParam = y

Mouse messages now follow the Win32/MSDN style:
  WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_LBUTTONUP / etc.
    wParam = MK_* button/modifier state
    lParam = MAKELPARAM(clientX, clientY)

  WM_MOUSEWHEEL
    wParam = MAKEWPARAM(MK_* state, wheelDelta)
    lParam = MAKELPARAM(screenX, screenY)

Added helpers/macros:
  MAKELPARAM
  GET_X_LPARAM
  GET_Y_LPARAM
  MK_LBUTTON / MK_RBUTTON / MK_SHIFT / MK_CONTROL / MK_MBUTTON
  WHEEL_DELTA
  ScreenToClient

The app/lab mouse handlers were updated to read coordinates from lParam.
This is important for later compatibility with foreign Win32-style code.

2) Child -> Parent kernel syscall-lite bridge

v68 adds a first explicit syscall bridge from an OOP GUI child process back to
the parent/session kernel side. The child no longer has to pretend that kernel
objects are local-only for the covered calls.

New IPC operations:
  MYOS_IPC_OP_KERNEL_REQUEST = 20
  MYOS_IPC_OP_KERNEL_ACK     = 21

Supported kernel bridge operations:
  MYOS_KOP_CREATE_EVENT
  MYOS_KOP_OPEN_EVENT
  MYOS_KOP_SET_EVENT
  MYOS_KOP_RESET_EVENT
  MYOS_KOP_CREATE_MUTEX
  MYOS_KOP_RELEASE_MUTEX
  MYOS_KOP_CREATE_SEMAPHORE
  MYOS_KOP_RELEASE_SEMAPHORE
  MYOS_KOP_CLOSE_HANDLE
  MYOS_KOP_WAIT_ONE
  MYOS_KOP_WAIT_MANY

Child-side wrappers currently included in myos_apphost_child.c:
  CreateEventA
  OpenEventA
  SetEvent
  ResetEvent
  WaitForSingleObject
  WaitForMultipleObjects
  CloseHandle

Parent-side behavior:
  processhost.c receives KREQ messages.
  It enters the child's myOS process context via MyWinEnterProcessContext(childPid).
  The real Object Manager / per-process handle table code then creates/opens/waits/closes the object.
  The child receives a KACK with the resulting handle/status/result.

This means the returned handles are allocated in the child process' myOS handle table,
not just in a random parent-global fake slot.

3) IPC Proxy diagnostics

The OOP proxy window now shows kernel bridge diagnostics:
  kreq op / req / ack / ok / result
  kernel_status text

The generic ipc-gui child runs a small self-test after its IPC window has been created:
  CreateEventA(Local\\myos.v68.child.kernelbridge)
  SetEvent
  WaitForSingleObject
  OpenEventA
  WaitForMultipleObjects
  ResetEvent
  CloseHandle

Limits still intentionally present
----------------------------------

This is not yet a full NT syscall table.
The bridge is scalar/shared-memory based and intentionally small.
The Wait model is still dispatcher-lite / polling-ish on the parent side.
Security descriptors are still Lite, not real SID/ACE/DACL/SACL Windows security descriptors.
PE loading is still not implemented here.
