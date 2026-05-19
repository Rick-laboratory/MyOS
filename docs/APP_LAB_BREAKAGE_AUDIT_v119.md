# myOS v119 App/Lab Breakage Audit

Purpose: mark the places most likely to fail during the ongoing MSDN-contract refactor. This audit is focused on apps and labs, not the core kernel32/user32 internals. The comments use `AUDIT(v119-app)`, `AUDIT(v119-lab)` and `AUDIT(v119-oop)` markers.

## Rule for triage

Do not treat every app/lab regression as an immediate blocker.

**Blocker**
- Build fails.
- `--smoke all` fails.
- Desktop cannot start.
- Shell/start/taskbar/message pump freezes globally.
- One app crash corrupts parent/shell state.

**Regression but not always blocker**
- Calc button path regresses.
- A lab control does not emit the expected `WM_COMMAND`.
- Dialog tab order or default button is wrong.
- Popup menu escape/submenu behavior is wrong.
- A diagnostic lab reports wrong state but the desktop stays usable.

**Expected compliance debt**
- Security/access lab fails because tokens/DACLs are not full MSDN yet.
- ServiceLab behavior changes while SCM becomes process-backed.
- Spy breaks after real session/desktop isolation.
- Childhost breaks while parent-child ABI moves from duplicated stubs to SDK/import-table style.

## App/Lab map

| File | Role | Likely first breakage | What it means |
|---|---|---|---|
| `app_calc.c` | Normal app / click canary | first-click lost, resize hit-test mismatch | Focus/capture/coordinate routing or custom-button shortcut drifted. |
| `app_editor.c` | Custom text client | keyboard/caret/scroll changes | Not a real EDIT control yet; strict WM_CHAR/focus may require rewrite. |
| `app_dialoglab.c` | Highest-value USER32 dialog canary | modal disable, tab, arrows, default button, COMDLG | Queue/focus/dialog-manager/menu/common-dialog contract drift. |
| `app_controllab.c` | Control routing lab | child focus, `WM_COMMAND`, synthesized notifications | Local ControlLite state diverged from real USER32 controls. |
| `app_waitlab.c` | Kernel32/Object/Process canary | waits, refs, duplicate handles, child process waits | Handle ownership/access, wait-state or process-host semantics changed. |
| `app_object.c` | Object/handle microscope | enumeration wrong/missing | Diagnostic API moved or handle table visibility changed; keep privileged. |
| `app_servicelab.c` | SCM contract lab | status/start/stop/query mismatch | SCM moved from table-backed to process-backed service model. |
| `app_pump.c` | Message queue stress lab | stalls, queue depth wrong, timer/self-post mismatch | Per-thread queue/WM_QUIT/filtering/cross-thread post semantics changed. |
| `app_deadlock.c` | SendMessageTimeout/hung-window lab | target not found, timeout behavior changes | SendMessage/timeout/reentrancy/pump rules are changing. |
| `app_draglab.c` | Capture/input routing lab | outside-window drag stops | SetCapture/ReleaseCapture or receiver-specific ScreenToClient changed. |
| `app_clipmenu.c` | Clipboard/menu/accelerator lab | popup/submenu/escape or clipboard lifetime | TrackPopupMenu not yet true modal loop; GlobalAlloc clipboard ownership transitional. |
| `app_paintlab.c` | GDI paint canary | invalidation, GetDC/BeginPaint mismatch, brush lifetime | HDC/object/region/surface model is becoming stricter. |
| `app_access.c` | Security/access probe | access denied/allowed mismatch | SecurityDescriptor/token/DACL work is intentionally incomplete. |
| `app_section.c` | Section/file mapping probe | view contents/access/lifetime mismatch | Mapping protection, view lifetime, inheritance or namespace got stricter. |
| `app_sharedbus.c` | Section + signal IPC probe | shared data wrong vs signal lost | Separate section data-plane bugs from message/signal bugs. |
| `app_spy.c` | Privileged diagnostic app | fails under isolation | Expected when session/desktop/security isolation gets real. |
| `terminal.c` | Shell/admin terminal | keyboard/process output/spawn issue | Shell/process integration or top-level message pump changed. |
| `myos_apphost_child.c` | OOP child runtime | child window/message/GDI/menu ABI drift | Parent-child IPC/ABI issue, not necessarily local USER32 failure. |

## Manual test order after major refactors

1. Boot desktop and open/close Start menu.
2. Open Calc and click a few digits/operators.
3. Open DialogLab: modal dialog, controls dialog, button dialog, tab/arrows/default button, Open/Save/ChooseFont.
4. Open ControlLab: button, edit, listbox, combo, scrollbar.
5. Open WaitLab + ObjectLab: create event, duplicate/open, wait any/all, child process wait.
6. Open ServiceLab: open SCM, create/start/query/stop/delete.
7. Open DragLab and test capture outside window.
8. Open ClipMenuLab and test accelerator/menu/clipboard.
9. Open PaintLab and test invalidate/GetDC/brush stress.
10. Open Spy last; it is privileged/internal and should not block a release unless it crashes the shell.

## Current expectation

v119 does not change runtime behavior. It only adds app/lab breakage markers and keeps the v118 core audit markers. Build and smoke must remain unchanged: `PASS` with 0 failures. Known WARNs from v118 remain compliance TODOs.
