myOS v33 - Clipboard + Menus + Accelerators

Adds a USER32-style interaction layer on top of the v31/v32 object/namespace/access base.

New API surface:
- GlobalAlloc / GlobalLock / GlobalUnlock / GlobalFree
- OpenClipboard / CloseClipboard / EmptyClipboard
- SetClipboardData(CF_TEXT) / GetClipboardData(CF_TEXT)
- IsClipboardFormatAvailable(CF_TEXT)
- CreateMenu / CreatePopupMenu / AppendMenuA / SetMenu / TrackPopupMenu-lite
- CreateAcceleratorTableA / TranslateAcceleratorA / DestroyAcceleratorTable
- WM_COMMAND

New app:
- Start menu -> ClipMenuLab
- Buttons: Set Clip, Get Clip, Clear, Attach Menu, Popup, Ctrl+C, Ctrl+V, Ctrl+N
- Keyboard accelerators: real Ctrl+C/Ctrl+V/Ctrl+N via TranslateAcceleratorA when ClipMenuLab is focused.

Notes:
- This is USER32-lite, not full Win32 binary compatibility.
- Menus are represented as USER handles and are visible/testable inside ClipMenuLab; desktop-level drop-down drawing is intentionally still lite.
- Clipboard currently supports CF_TEXT.
