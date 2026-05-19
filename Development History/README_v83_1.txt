myOS v83.1 - Alt+F4 SysCommand Fix

BUILD: myos_v83_1_alt_f4_syscommand_fix

Fix:
- Alt+F4 is now handled before the legacy F4 debug launcher.
- Alt+F4 posts/runs WM_SYSCOMMAND / SC_CLOSE for the active top-level window.
- Alt+Space still opens the system menu via WM_SYSCOMMAND / SC_KEYMENU.
- Plain F4 without Alt still keeps the old debug terminal launcher.

Test:
1. sudo chvt 3
2. sudo ./myos_input /dev/input/event1 /dev/input/event2
3. Open Calc or Editor.
4. Press Alt+F4: active app should close, not open a new terminal.
5. Press plain F4: debug terminal shortcut may still open a terminal.
6. Press Alt+Space: system menu should open for the active window.
7. Use the system menu Close item: it should take the same WM_SYSCOMMAND / SC_CLOSE path.
