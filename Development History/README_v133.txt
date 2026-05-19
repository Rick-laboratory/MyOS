BUILD: myos_v133_shell_broker_dispatch_context_fix

Purpose:
- Fix regression introduced by v131 HWND access hardening where shell/Desktop/Taskbar queued messages could be pumped from a neutral/non-shell ambient Capability.
- queued HWND dispatch now enters the target window owner runtime context before invoking the target WndProc.
- keeps v131 injection blocking intact: foreign PostMessage/SendMessage/forged Dispatch still cannot mutate foreign USER32/MDI state.

User-visible fixes expected:
- Start/right-click menu item execution works again.
- Desktop single-click selection and double-click open paths work again through DesktopWndProc.
- Taskbar/minimized-window resume works again through Shell_TrayWnd.

Verification:
- make clean && make -j2
- ./myos_input --smoke shell_broker
- ./myos_input --smoke all
