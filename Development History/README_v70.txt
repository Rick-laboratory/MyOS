myOS v70 - OOP WaitLab/ObjectProbe + DuplicateHandle Kernel Bridge
===================================================================

BUILD: myos_v70_oop_wait_objectlab_duplicatehandle

Main changes since v69:

1) WaitLab is now an out-of-process GUI app by default
   - Start menu / apphost image "wait-lab" launches myos_apphost_child.
   - The child owns the WndProc/message loop and renders through the generic
     GDI IPC command buffer.
   - Buttons exercise the real child->parent Kernel Bridge:
       CreateEventA, OpenEventA, SetEvent, ResetEvent,
       WaitForSingleObject(0/1000/INFINITE), DuplicateHandle, CloseHandle.
   - Shared object name used by all WaitLab instances:
       Global\myos.v70.waitlab.event

2) Cross-process wait test is now visible
   - Open two WaitLab windows.
   - In A click Create/Reset, then WaitInf.
   - In B click Open, then Set.
   - A should wake without freezing the parent/session because v69's worker
     KREQ execution keeps ProcessHost IPC drainable while A is waiting.

3) ObjectLab OOP probe
   - "object-lab" now launches ObjectProbe [OOP] by default.
   - It creates/opens/duplicates/sets/closes the same Global v70 event through
     the Kernel Bridge, so Object/handle table effects happen under the child
     myOS PID.
   - The old in-process enumerating ObjectLab is still available as:
       object-lab-classic
   - The old in-process WaitLab is still available as:
       wait-lab-classic

4) DuplicateHandle bridge
   - New KREQ op: MYOS_KOP_DUPLICATE_HANDLE.
   - Child-side DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), ...)
     marshals through the Kernel Bridge.
   - Parent executes DuplicateHandle under MyWinEnterProcessContext(childPid),
     so the duplicated handle is allocated in the child's real per-process
     handle table.

5) Version metadata
   - IPC/shared versions bumped to 70.
   - Build labels updated to v70.

Build:
    make clean && make

Run as before:
    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2

Limitations:
   - ObjectProbe OOP does not enumerate the global Object Manager from inside
     the child yet. It is a real object/handle producer. Use object-lab-classic
     or Spy/ProcessHost diagnostics to observe the produced handles.
   - DuplicateHandle v70 supports self-duplication in the calling child process.
     Cross-process source/target PID duplication is reserved in the shared ABI
     but not exposed as a UI flow yet.
