myOS v192 - dialog message semantics

Focus:
- Harden IsDialogMessageA/W for real Win32-style dialog keyboard routing.
- Accept both Linux evdev KEY_* values and Win32 VK_* values for TAB/ENTER/ESC/arrows.
- Preserve control-local dispatch using the current internal KEY_* messages.
- Add WM_SYSCHAR mnemonic handling and W alias export.
- Smoke covers disabled/hidden tabstop skipping, DM_SETDEFID/DM_GETDEFID, VK_RETURN/VK_ESCAPE, and mnemonic activation.
