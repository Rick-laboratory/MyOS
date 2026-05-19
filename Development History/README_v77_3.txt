myOS v77.3 - app-client coordinate/capture fix

Problem found after v77 shell-HWND migration:
- START/Desktop/Taskbar became real HWNDs.
- Minimize/close still worked through the legacy non-client path.
- App client clicks reached either the wrong/stale capture target or used fragile generic ScreenToClient conversion, so OOP apps reported clicks outside their buttons.

Fixes:
1. post_client_mouse_message now clears invalid/stale shell capture instead of black-holing client clicks.
2. Top-level app HWND lParam is computed directly from WindowManager frame coordinates:
   clientX = screenX - (window.x + 1)
   clientY = screenY - (window.y + TITLEBAR_H)
3. Child HWNDs still use ScreenToClient(child), preserving real child-control routing.
4. v77 shell classes remain intact: #32769, Shell_TrayWnd, START BUTTON.

Test procedure:
1. sudo chvt 3
2. sudo ./myos_input /dev/input/event1 /dev/input/event2
3. Open Calculator from START menu.
4. Click calculator buttons: last button / hit counter / display must update.
5. Open Editor and click into the text/client area, then type: caret/text focus should react again.
6. Open HWND StateProbe/SurfaceLab and click their in-client buttons.
7. START button, Desktop right-click menu, minimize and close must still work.

Expected result:
- App-client clicks are no longer swallowed.
- Apps receive WM_LBUTTONDOWN with MSDN-style client-relative lParam.
- v77 shell-HWND architecture stays active.
