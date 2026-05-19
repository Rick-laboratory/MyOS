# myOS v118 Breakage Audit

Stand: based on `myos_v117_header_contract_cleanup` after full source pass.

This pass intentionally does **not** add runtime features. It adds upfront breakage notes so we can keep moving fast without losing track of where the old implementation style will likely crack once we push harder toward strict MSDN compliance.

## Gate status

```txt
BUILD: PASS
SMOKE: PASS
SMOKE RESULT: PASS (0 failures)
```

The audit comments use the marker:

```c
/* AUDIT(v118): ... */
```

## Severity legend

```txt
BLOCKER      Build/smoke/desktop can break or process isolation can be violated.
HIGH         Will break real Win32/MSDN compatibility once foreign code leans on it.
MEDIUM       Works for current labs but likely breaks under load or edge cases.
LOW          Cosmetic/diagnostic/versioning debt.
```

## Highest-risk items upfront

### 1. Public SDK still leaks private layout

Severity: **HIGH**

Files:

```txt
sdk/include/windef.h
sdk/include/winuser.h
sdk/include/winsvc.h
```

Why it will break:

`windef.h` still includes `../../mytypes.h`, and `winuser.h` still includes `../../myqueue.h`. Public `MSG` also carries `MyMessage _myos`. That is extremely useful for the current runtime because `DispatchMessageA()` can dispatch the exact queue payload, but it is not strict WinSDK layout.

Expected failure mode:

External code that assumes MSDN `sizeof(MSG)` or constructs a `MSG` manually will not behave like Windows. The current `DispatchMessageA()` depends on `MSG._myos`, so a manually constructed `MSG` with only `hwnd/message/wParam/lParam` can dispatch nothing or dispatch stale private payload.

When it matters:

As soon as we compile foreign Win32-style apps or make SDK headers a real external contract.

Recommended later fix:

Introduce strict public `MSG`, keep the private queue payload in a sidecar structure or internal lookup keyed by sequence/thread queue. `DispatchMessageA()` must dispatch from public fields when no sidecar exists.

---

### 2. `DispatchMessageA()` relies on private `MSG._myos`

Severity: **HIGH**

File:

```txt
winuser.c
```

Why it will break:

`PeekMessageA()`/`GetMessageA()` fill `MSG._myos`, then `DispatchMessageA()` calls `hwnd_dispatch_message()` with that private copy. That is fine for messages that came from the myOS queue. It is not MSDN-compliant for manually prepared MSG values, and it makes the private `MSG` layout part of the ABI.

Expected failure mode:

Synthetic tests, accelerator/menu/dialog code, or foreign apps that call `DispatchMessageA(&msg)` after constructing `msg` manually may silently do nothing or target the wrong stale payload.

Recommended later fix:

Make `DispatchMessageA()` resolve `lpMsg->hwnd` and call the window proc from public fields directly, using private sidecar data only as an optimization/path-preservation mechanism.

---

### 3. Raw-handle compatibility fallback weakens process isolation

Severity: **BLOCKER/HIGH**

File:

```txt
winbase.c
```

Why it will break:

`mywin_resolve_handle()` returns raw handles when a process handle-table entry is not found. `mywin_has_handle_access()` also accepts raw non-user handles as valid old compatibility handles. This keeps old demos alive, but it is the biggest long-term contradiction to strict per-process handle tables.

Expected failure mode:

A stale or foreign raw object handle may bypass handle table access checks. Access-denied tests can pass through accidentally. Process isolation looks correct for table handles but becomes fuzzy through legacy paths.

Recommended later fix:

Add a strict mode: public KERNEL32 APIs must require a per-process handle table entry. Old raw handles may only be accepted by explicit `My*` diagnostic/internal APIs.

---

### 4. LastError is still not consistently set

Severity: **HIGH**

Files:

```txt
winbase.c
winuser.c
wingdi.c
winsvc.c
commdlg.c
```

Why it will break:

A lot of Win32 APIs return `FALSE`, `NULL`, `0`, or `WAIT_FAILED` without calling `SetLastError()`. Smoke already warns for `CloseHandle(NULL)` and `WaitForSingleObject(NULL)`.

Expected failure mode:

Foreign code will branch on `GetLastError()` and make the wrong decision. This also hides real bugs because stale error codes survive through failure paths.

Recommended later fix:

Every public API entry should have a defined error path. Add smoke cases that assert `ERROR_INVALID_HANDLE`, `ERROR_INVALID_PARAMETER`, `ERROR_ACCESS_DENIED`, `ERROR_ALREADY_EXISTS`, `ERROR_NOT_ENOUGH_MEMORY`, etc.

---

### 5. Wait model is polling and non-atomic

Severity: **HIGH**

File:

```txt
winbase.c
```

Why it will break:

`WaitForSingleObject()` and `WaitForMultipleObjects()` use polling with `usleep(1000)`. `WAIT_ALL` probes objects once without consuming, then consumes one by one. That is not atomic across auto-reset events/semaphores/mutexes.

Expected failure mode:

Under concurrency, `WAIT_ALL` may observe all objects ready and then consume a set that changed underneath it. Auto-reset objects and semaphores are the danger zone. There is also no abandoned-mutex result behavior yet.

Recommended later fix:

Create a dispatcher object layer with wait queues/condition variables or Linux `eventfd`/`futex` style wakeups. Implement atomic `WAIT_ALL` acquisition and `WAIT_ABANDONED_*`.

---

### 6. USER32 global state is not thread/desktop/session safe yet

Severity: **HIGH**

File:

```txt
winuser.c
```

Why it will break:

Classes, capture, focus, dialogs, templates, menus, globals, accelerators and clipboard are process/global arrays with limited locking. Some state is global where Windows behavior is thread/desktop/session scoped.

Expected failure mode:

Multiple UI threads, multiple desktops/sessions, nested modal loops, or OOP windows from different processes can steal focus/capture/clipboard/menu state from each other.

Recommended later fix:

Move capture/focus/queues to thread or desktop-owned state. Keep global tables only for intentionally global/session objects.

---

### 7. Fixed-size object tables will produce silent edge failures

Severity: **MEDIUM/HIGH**

Files:

```txt
winuser.c
winbase.c
wingdi.c
winsvc.c
processhost.c
window.h
myobject.h
process_ipc.h
```

Current caps include:

```txt
MAX_WINDOWS=16
MYWIN_MAX_WINDOW_INFOS=128
MYWIN_MAX_DIALOGS=16
MYWIN_MAX_EVENTS=64
MYWIN_MAX_HANDLE_TABLE_ENTRIES=256
_OBJECT_MAX_OBJECTS=256
MYGDI_MAX_COMMANDS=768
MYPROCESSHOST_MAX=64
MYOS_IPC_MAX_CHILD_CONTROLS=8
```

Why it will break:

The current labs fit. Real app suites, repeated start/close cycles, complex dialogs, popup-heavy menus, or MDI will hit these caps.

Expected failure mode:

Functions start returning `0`/`FALSE`, sometimes without LastError. Worse: some capacity failures are visually indistinguishable from routing bugs.

Recommended later fix:

Convert core registries to growable vectors or object pools with clear `ERROR_NOT_ENOUGH_MEMORY`/`ERROR_NOT_ENOUGH_QUOTA` paths.

---

### 8. OOP child runtime duplicates WinAPI typedefs/constants locally

Severity: **MEDIUM/HIGH**

File:

```txt
myos_apphost_child.c
```

Why it will break:

The child runtime has its own local `DWORD`, `HWND`, `MSG`, `WNDCLASSEXA`, constants, and tiny USER32/KERNEL32 stubs. That is currently useful because child mode is self-contained. But it will drift from `sdk/include/*.h` as the SDK improves.

Expected failure mode:

A parent-side signature or message layout fix compiles, smoke passes for in-process apps, but OOP apps still use old ABI assumptions.

Recommended later fix:

Split the child runtime into a tiny `child_user32.c` / `child_winbase.c` that includes the real SDK headers, then only overrides transport implementation behind the same signatures.

---

### 9. `CreateWindowExA()` / `RegisterClassExA()` are happy-path strictness gaps

Severity: **MEDIUM/HIGH**

File:

```txt
winuser.c
```

Why it will break:

Class registration ignores many MSDN fields/validation details and uses one global class table. `CreateWindowExA()` requires runtime binding/capability and does not consistently set LastError. It also keeps fallback thunks in a static ring.

Expected failure mode:

Foreign code relying on class atoms, class styles, `cbWndExtra`, `hbrBackground`, ownership, parent/child edge cases, or creation failure errors will diverge from Windows.

Recommended later fix:

Per-process/per-module class registration, strict class atom/name behavior, full `CREATESTRUCT`, `WM_NCCREATE/WM_CREATE/WM_NCDESTROY` failure cleanup, proper `cbWndExtra`, consistent LastError.

---

### 10. Window geometry is split between USER32 metadata and WindowManager slots

Severity: **MEDIUM/HIGH**

Files:

```txt
winuser.c
window.c
window.h
```

Why it will break:

Standalone `CreateWindowExA()` HWNDs have USER32 metadata, but `GetWindowRect`, `MoveWindow`, and `SetWindowPos` currently need WindowManager-backed top-level slots except for some dialog paths.

Expected failure mode:

Smoke already warns. Apps that create top-level USER32 windows outside the shell/window-manager path cannot reliably move/query them.

Recommended later fix:

Make every top-level HWND have a compositor/window-state entry or make WindowManager derive from USER32 top-level state, not the other way around.

---

### 11. `TrackPopupMenu()` modal loop was only a deterministic shortcut before v154

Severity: **LOW/MEDIUM after v154**

File:

```txt
winuser.c
```

v154 status:

The old pre-init first-command shortcut has been replaced by a small modal popup tracker: owner init runs before item resolution, queued UP/DOWN/ENTER/ESC and mouse-row commits are handled, `TPM_NONOTIFY` and `TrackPopupMenuEx/TPMPARAMS` are smoke-gated, and capture/focus are restored. Remaining gaps are real rendered hover tracking, full submenu geometry, work-area/exclude-rect placement and deep animation/alignment flags.

Expected failure mode:

Simple popup demos pass and basic modal-loop semantics are closer to Win32, but complex owner-drawn/nested/positioned menus will still expose PoC limitations.

Recommended later fix:

Integrate HMENU tree with the existing WindowManager app popup state and formalize modal menu loop/capture/cancel behavior.

---

### 12. Clipboard/GlobalAlloc ownership is only session-scalar

Severity: **MEDIUM**

File:

```txt
winuser.c
```

Why it will break:

Clipboard uses one global owner/data/format and simple global memory slots. Windows clipboard ownership/format enumeration/lifetime has more rules. `SetClipboardData()` ownership transfer is especially easy to get wrong.

Expected failure mode:

Multiple formats, delayed rendering, owner death, or apps freeing handles after setting clipboard data will diverge.

Recommended later fix:

Move clipboard to a session object with format table and formal ownership-transfer semantics.

---

### 13. GDI is command-buffered and limited

Severity: **MEDIUM**

File:

```txt
wingdi.c
```

Why it will break:

The current GDI model is good for PaintLab: DCs, brushes, dirty regions, and command buffering. It is not yet a full GDI object model. There are no compatible DCs/bitmaps/fonts/pens/regions/clipping/ROP/BitBlt contract details.

Expected failure mode:

Drawing-heavy Win32 code will compile but render incorrectly or exhaust command/DC/brush tables.

Recommended later fix:

Introduce real GDI object table by type, selected object lifetimes, compatible bitmap/DC path, clip regions, and BitBlt.

---

### 14. SCM public header still exposes private `MyServiceInfo`

Severity: **MEDIUM/HIGH**

Files:

```txt
sdk/include/winsvc.h
winsvc.c
```

Why it will break:

MSDN uses `SERVICE_STATUS`, `SERVICE_STATUS_PROCESS`, service handles and service-control dispatcher APIs. Current public header exposes `MyServiceInfo` directly.

Expected failure mode:

Foreign service code cannot compile against real Win32 expectations, and internal fields become ABI.

Recommended later fix:

Add `SERVICE_STATUS`; make `QueryServiceStatus()` and `ControlService()` use that. Move `MyServiceInfo` to `myos_diag.h` or `myos_private.h`.

---

### 15. Session Manager is still conceptual, not a boot owner

Severity: **MEDIUM/HIGH**

Files:

```txt
main.c
winsvc.c
myobject.c
winbase.c
window.c
```

Why it will break:

The namespace mentions sessions and services, but no real `smss`-style owner initializes sessions, window stations, desktops, SCM, object namespace, and apphost as separate boot phases.

Expected failure mode:

Global singletons keep working while one desktop exists. They become messy when we add multiple sessions/desktops or want service isolation.

Recommended later fix:

Build `smss-lite` as the first owner of namespace/session/SCM/desktop/apphost boot order.

---

## Suggested next work order

```txt
v119: LastError + invalid handle strictness
v120: strict handle-table mode behind a build/runtime flag
v121: public MSG strictness + DispatchMessage fallback from public fields
v122: WindowManager/User32 geometry unification
v123: OOP child runtime includes real SDK headers
v124: SERVICE_STATUS / winsvc public contract cleanup
v125: menu modal loop + TrackPopupMenu real behavior
```

## Useful manual test triggers

Use these to force the weak points intentionally:

```txt
- open >16 top-level windows
- create >16 dialogs or nested modals
- add >128 listbox/combo items
- create >32 menus or >32 items per menu
- create >64 events/sections/brushes repeatedly
- manually construct MSG and call DispatchMessageA
- OpenEventA(SYNCHRONIZE) then SetEvent(handle): must fail with ERROR_ACCESS_DENIED
- CloseHandle(NULL), WaitForSingleObject(NULL): must set ERROR_INVALID_HANDLE
- CreateWindowExA top-level then GetWindowRect/MoveWindow without WindowManager slot
- run two OOP apps that both use clipboard/menu/kernel IPC stubs
```
