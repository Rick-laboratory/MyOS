myOS v36 - Mouse Capture + DragLab
==================================

Added:
- SetCapture(HWND)
- ReleaseCapture()
- GetCapture()
- WM_CAPTURECHANGED
- captured mouse routing: WM_MOUSEMOVE / WM_LBUTTONUP continue to captured HWND even when the cursor leaves the client area
- DragLab app in Start menu and optional F15 hotkey

DragLab tests:
- Capture: calls SetCapture manually
- Release: calls ReleaseCapture manually
- Drag the DRAG ME box; movement is still received while the cursor leaves the client because capture owns the mouse stream
- Drop onto target increments drop counter
- Cancel: cancels capture + active drag

Build markers:
- HUD: v36 CAPTURE+DRAGLAB
- desktop marker: BUILD v36: capture + DragLab
- runtime: BUILD: myos_v36_capture_draglab - mouse capture + DragLab
