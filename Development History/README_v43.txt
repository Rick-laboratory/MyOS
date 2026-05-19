myOS v43 - USER32 WM_CLOSE / WM_QUIT Lifecycle

Goal
----
Make closing windows follow the Win32 message path instead of having the
WindowManager directly tear down HWNDs. v41/v42 established creation and
tree lifetime; v43 connects the shell close button to WM_CLOSE and adds
message-loop quit semantics.

Changes
-------
* DefWindowProcA(WM_CLOSE) now calls DestroyWindow(hWnd).
* WindowManager close button now posts WM_CLOSE to the window owner queue.
* Apps can intercept WM_CLOSE and choose not to close.
* DestroyWindow has a reentrancy guard: recursive DestroyWindow during
  WM_DESTROY is treated as a no-op success.
* DestroyWindow keeps the desktop WindowManager slot in sync when an app
  closes itself through USER32.
* PostQuitMessage(int exitCode) posts WM_QUIT to the current UI thread.
* GetMessageA returns FALSE when it removes WM_QUIT, matching Win32
  message-loop convention.

Important path
--------------
  shell close button
    -> PostMessage(hwnd, WM_CLOSE, 0, 0)
    -> app WndProc may handle or call DefWindowProcA
    -> DefWindowProcA(WM_CLOSE)
    -> DestroyWindow(hwnd)
    -> child HWNDs destroyed first
    -> WM_DESTROY while WindowLong/UserData still exists
    -> HWND table removal
    -> WM_NCDESTROY
    -> desktop slot marked closed

Build/Test
----------
  make clean && make
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Recommended tests
-----------------
* Open ServiceLab, click controls, close with X, reopen.
* Open ControlLab/ClipMenuLab, close with X, reopen.
* Open multiple windows, close foreground and verify focus moves to the next.
* Confirm upper-left build marker says v43.

Known boundary
--------------
WM_QUIT now exists and GetMessageA honors it, but full app-thread exit /
PostQuitMessage-from-WM_DESTROY patterns are still limited by the current
Lite-process model.
