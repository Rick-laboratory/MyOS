myOS v62 - First real out-of-process GUI app: Calculator

BUILD: myos_v62_oop_calc_gui_ipc

What changed:
- Calculator aliases (calc/calc.exe/calculator/calculator.exe) now use the gui-ipc subsystem.
- The calculator WinMain/WndProc/state run inside the real fork/exec Linux child (myos_apphost_child).
- The child uses the v61 user32-like IPC runtime stubs:
  RegisterClassExA -> CreateWindowExA -> PostMessageA -> GetMessageA -> DispatchMessageA.
- Parent WindowManager stays the Session/Desktop process and creates the HWND/frame.
- Calculator display/operator/history/button-state are published through the ProcessHost shared section.
- Parent paints the calculator UI from the child-owned state snapshot.
- Mouse clicks are delivered as WM_LBUTTONDOWN over the cross-process queue and processed inside the child WndProc.
- WM_CLOSE goes to the child and the Linux process exits with code 62.

Smoke test performed:
- MyAppHostLaunch("calc") creates a Process-Lite + Linux child.
- ProcessHost reports calc_enabled=1 and display="0".
- Parent posts WM_LBUTTONDOWN to the HWND.
- Child DispatchMessage/WndProc updates display to "7" and button_hits=1.
- Parent WM_CLOSE routes to child, child exits, ProcessHost reaps exit code 62.

Limitations intentionally left for later:
- Rendering is still parent-side framebuffer drawing from shared state, not a real per-process shared render surface.
- Only Calculator is ported; editor/paint/control/etc. are still in-process.
- The child does not yet own GDI drawing commands or a real surface swapchain.
