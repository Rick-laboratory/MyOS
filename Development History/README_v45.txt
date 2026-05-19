myOS v45 - process runtime context boundary
===========================================

BUILD:
  myos_v45_process_runtime_context

Goal
----
v44 made PROCESS/THREAD objects waitable and added TerminateProcess/GetExitCodeProcess.
v45 adds the next missing substrate piece before a real loader: a switchable
per-process runtime context.

What changed
------------
1) Runtime context stack
   New APIs:
     BOOL  MyWinEnterProcessContext(DWORD dwProcessId);
     BOOL  MyWinLeaveProcessContext(void);
     DWORD MyWinGetRuntimeContextDepth(void);
     BOOL  MyWinGetRuntimeContextInfo(DWORD pid, MyRuntimeContextInfo* out);

   EnterProcessContext switches the current WinAPI identity to the target
   process-lite capability. GetCurrentProcessId(), handle allocation,
   object ownership and CreateEvent/CreateMutex/CreateSection calls now run
   under that process identity until MyWinLeaveProcessContext restores the
   caller.

2) Child process capability envelope
   CreateProcessA() now gives the child its own Capability:
     child.cap = parent.cap
     child.cap.id = childPid
     child.cap.name = imageName

   This is not exec/fork isolation yet. It is the runtime identity envelope
   the later loader/apphost will use.

3) Stronger pid-local handle isolation
   Normal processes no longer accidentally resolve another process' handle
   value through the debug fallback path. Only full CAP_ADMIN callers may
   resolve foreign pid-local handles directly. This caught an old bug where
   "flags & CAP_ADMIN" treated almost every capability as admin. v45 now
   requires:
     (flags & CAP_ADMIN) == CAP_ADMIN

4) WaitLab test button
   WaitLab has a new button:
     ChildCtx

   Test flow:
     Spawn Child
     ChildCtx

   Expected status:
     parent=<waitlab pid>
     enter PID=<child pid>
     CreateEvent -> child-handle=...
     ownerPID=<child pid>
     childHandles increases
     enters increases

   This proves a WinAPI call executed under the child's process context and
   allocated the handle into the child's pid-local handle table.

5) Process/Runtime diagnostics
   MyProcessLiteInfo now includes:
     cap_flags
     runtime_enters
     runtime_depth
     cap_name

Smoke test performed
--------------------
A separate C smoke test linked against the built objects and verified:
  - parent runtime bind works
  - CreateProcessA creates child PID 3000
  - MyWinEnterProcessContext(child) makes GetCurrentProcessId() return child PID
  - CreateEventA while inside child context creates a handle owned by child PID
  - MyWinLeaveProcessContext restores parent PID
  - parent cannot SetEvent() or WaitForSingleObject() on the child-local handle
  - TerminateProcess(child,45) makes the process handle waitable/signaled
  - GetExitCodeProcess returns 45

Still not real yet
------------------
This is not a real loader and not Linux process isolation yet. Apps still run
inside the host process. But the WinAPI runtime state now has a real process
identity switch boundary, which is the necessary bridge toward:
  v46 AppHost / loader process table
  v47 IPC boundary for USER32/Object calls
  v48 real app process split on Linux fork/exec or helper-host model
