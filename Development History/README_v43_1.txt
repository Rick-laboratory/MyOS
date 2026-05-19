myOS v43.1 - WM_CLOSE calculator fix

Patch release on top of v43.

Fix:
- Shell close button now delivers WM_CLOSE synchronously via hwnd_send_timeout().
- Calculator and other USER32-backed windows close immediately again.
- Apps still retain Win32-style first refusal: handling WM_CLOSE without DestroyWindow() keeps the window alive.

Build:
  make clean && make

Run:
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Regression tests:
- Open Calculator, click X: window disappears.
- Terminal click X: still closes.
- ServiceLab click X: still closes, child BUTTON HWNDs are destroyed with parent.
