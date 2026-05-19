myOS v31/v32 - Access Checks + Namespace Directories

v31:
- Handle granted-access checks for wait/set/reset/map/release/timer operations.
- SYNCHRONIZE-only handles can wait but cannot SetEvent.
- ObjectLab shows SD flags and namespace id.

v32:
- Named objects are canonicalized into NT-ish namespaces:
  Local\foo  -> \Sessions\1\BaseNamedObjects\foo
  Global\foo -> \BaseNamedObjects\foo
- Sections, Events, Mutexes, Semaphores and Timers use canonical names for create/open.

WaitLab additions:
- RO Open: OpenEventA(SYNCHRONIZE)
- RO Set: expected ACCESS_DENIED/FALSE
- RO Wait: expected normal wait/timeout using SYNCHRONIZE

Also fixed WaitLab log/status overlap from v30.
