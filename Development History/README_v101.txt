myOS v101 - interactive menubar and submenu navigation

BUILD: myos_v101_interactive_menubar_submenus

Main focus:
- Real HMENU-backed application menubar rendering for windows that call SetMenu(hwnd, hMenu).
- Interactive top-level menu bar hit-testing in the WindowManager.
- Popup menu navigation using real MENUITEMINFOA/GetMenuItemInfoA data.
- Submenu opening on hover, click, Right arrow, and Enter/Space.
- Disabled/gray/separator/checked/popup state is respected visually and during invocation.
- WM_ENTERMENULOOP / WM_EXITMENULOOP / WM_INITMENU / WM_INITMENUPOPUP / WM_UNINITMENUPOPUP / WM_MENUSELECT are sent to the owning app window.
- WM_COMMAND is sent to the app window for invokable menu items with lParam == 0.
- Alt release, Alt+mnemonic, and F10 activate the focused app menubar.
- Left/Right/Up/Down/Home/End/Escape/Enter/Space keyboard navigation implemented for active app menu loops.

Win32/MSDN compliance additions:
- MENUITEMINFOA
- GetMenuItemInfoA
- MIIM_* / MFT_* / MFS_* constants
- WM_UNINITMENUPOPUP constant

DialogLab test path:
1. Start DialogLab.
2. Press/click "Menu APIs Probe" once to create and attach the menu bar.
3. A File/View/OwnerDraw menu bar appears under the title bar.
4. Test mouse:
   - Click File.
   - Hover Recent to open the submenu.
   - Click Template One/Two or Open/Exit.
   - Hover File -> View -> OwnerDraw while a popup is open.
5. Test keyboard:
   - Press/release Alt, or press F10, or press Alt+F/Alt+V/Alt+O.
   - Use Left/Right/Up/Down/Home/End.
   - Right opens a submenu; Left closes a submenu or moves to previous top-level menu.
   - Enter/Space invokes the selected command.
   - Escape closes submenu/menu levels.

Notes:
- Start menu and system menu paths remain intact and continue using the existing shell menu table.
- The app menubar is compositor-owned chrome placed between title bar and client area, so menubar clicks are not routed as client WM_LBUTTONDOWN.
