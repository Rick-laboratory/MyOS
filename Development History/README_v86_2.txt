myOS v86.2 - START Button Direct Command Fix

BUILD: myos_v86_2_start_button_direct_command_fix

Fix:
- START remains a real BUTTON child HWND of Shell_TrayWnd.
- A deterministic raw-input fallback now invokes the Taskbar WM_COMMAND/ID_TASKBAR_START path synchronously when the click is inside the START rect.
- This avoids lost START clicks caused by stale shell child-hit/capture state.

Tests:
1. sudo chvt 3; sudo ./myos_input /dev/input/event1 /dev/input/event2
2. Click START -> menu opens.
3. Click START again -> menu closes.
4. START -> Neuer Rechner -> Calc opens.
5. Regression: ControlLab Tab/Space/Enter, Alt+Space, Alt+F4, app client clicks, move/resize.
