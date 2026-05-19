BUILD: myos_v146_runtime_guard_hardening

Purpose
-------
v146 is a stabilization pass after the v145 first-launch MDI drag fix.
It does not start a new feature pillar. It hardens the raw-input/runtime
contract so the same class of bug cannot come back through a different
thread-local capability state.

Background
----------
v145 fixed the manual bug where fresh boot -> first MDILab start -> MDI child
caption drag failed until Escape closed one child. Root cause: raw evdev/input
callbacks run on a thread-local USER32 runtime context. The input thread could
start without CAP_WINDOW_READ/CAP_WINDOW_CONTROL, so the first raw hit-test could
miss GetParent/GetWindowRect/GetWindow access until some other path primed TLS.

v146 extends that fix:

1. Centralized session-input runtime guard
   - New private helper:
       MyWinEnsureSessionInputRuntime(HWNDManager*, WindowManager*, const Capability*)
   - Raw compositor/input routes now go through one USER32-side runtime guard.
   - The helper binds the desktop and upgrades insufficient thread-local caps.

2. Harden against weak existing runtime state
   - v145 handled empty TLS.
   - v146 also handles a thread already bound to a normal app capability that
     lacks CAP_WINDOW_ENUM / CAP_WINDOW_READ / CAP_WINDOW_CONTROL /
     CAP_WINDOW_SUBSCRIBE.
   - Raw input is broker work, so it must run under the shell/session broker
     contract, not whatever app capability happened to be active.

3. Input thread uses the same guard
   - main.c::input_thread no longer duplicates the cap creation logic.
   - It calls MyWinEnsureSessionInputRuntime(&mgr, &wm, &wm.shell_cap).

4. New smoke coverage
   - Existing v145 no-runtime raw-input MDI caption drag remains.
   - New weak-runtime smoke binds a low-rights CAP_IPC-only capability in a fresh
     pthread, then performs raw MDI caption drag. This must upgrade to session
     broker and move the child.

Smoke highlights
----------------
[PASS] app_labs  MDILab first input-thread caption drag
[PASS] app_labs  MDILab weak-runtime caption drag
[PASS] app_labs  MDILab initial active caption drag
[PASS] app_labs  MDILab initial background caption drag
[PASS] app_labs  MDILab toolbar New one-shot
[PASS] app_labs  MDILab toolbar fast click no-pump
[PASS] app_labs  MDILab physical caption drag

Validation
----------
make clean && make -j2
./myos_input --smoke all

Result:
BUILD: myos_v146_runtime_guard_hardening
SMOKE RESULT: PASS (0 failures)
