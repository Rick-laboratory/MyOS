myOS v40.2 - USER32-style BUTTON controls

Goal:
- Move ServiceLab away from app-owned button draw/hit-test helpers.
- Controls are now HWND children created through CreateWindowExA("BUTTON", ...).
- BUTTON has its own WndProc and sends WM_COMMAND with BN_CLICKED to its parent.

Implemented:
- Built-in BUTTON class registered from MyWinBindRuntime().
- CreateWindowExA now stores WinAPI-ish window metadata: parent HWND, child ID, text, style, rect, class name.
- Added LOWORD/HIWORD/MAKEWPARAM and BN_CLICKED.
- Added GetParent(), GetDlgCtrlID(), ChildWindowFromPoint().
- Added MyDrawChildWindows() as a temporary framebuffer bridge until WM_PAINT/DC painting is fully control-owned.
- ServiceLab creates child buttons with CreateWindowExA("BUTTON", ... WS_CHILD|WS_VISIBLE ... hMenu=id).
- ServiceLab handles WM_COMMAND/BN_CLICKED instead of app-level button hit testing.

Important architectural note:
This is intentionally closer to WinAPI semantics than v40.1. The remaining non-WinAPI bridge is MyDrawChildWindows(), because the current desktop renderer still calls app-specific blit functions instead of driving every HWND via WM_PAINT/HDC. Next step should be replacing that bridge with real invalidation + WM_PAINT for child windows.

Build:
make clean && make
