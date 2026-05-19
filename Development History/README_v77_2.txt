myOS v77.2 - client mouse delivery fix

Fix after v77/v77.1 shell HWND refactor:
- Desktop (#32769), Taskbar (Shell_TrayWnd) and START button remain real HWNDs.
- Raw shell router no longer gets the final word on app client clicks.
- After the router focuses the app, main.c verifies whether the cursor is in the focused app client endpoint.
- If yes, WM_LBUTTONDOWN is posted to the real client HWND even if the shell/nonclient router reported handled.

Test:
1. sudo chvt 3
2. sudo ./myos_input /dev/input/event1 /dev/input/event2
3. Click calculator buttons in the client area. The display / hits / last button must update.
4. Open HWND StateProbe and test Map/Refresh/CloseMap buttons.
5. START button and desktop context menu must still work.
6. Titlebar drag/close/minimize should still work through the legacy nonclient path.
