myOS v191 - accelerator/modal pump unification

- USER32 modal loops now share a small common pump primitive: wait, WM_QUIT, TranslateAccelerator, IsDialogMessage, dispatch.
- DialogBox modal loop was switched onto this shared pump while preserving the short idle wait needed by the current in-process compositor path.
- TranslateAcceleratorA now accepts both Win32-style virtual-key values (A..Z/0..9) and the current desktop Linux KEY_* codes used by myOS input.
- Character accelerators without FVIRTKEY now work for WM_CHAR/WM_SYSCHAR.
- Modifier matching remains strict: Ctrl+N does not accidentally fire on Ctrl+Shift+N.
- Child AppHost accelerator shim now follows the same key normalization and Shift/Ctrl/Alt matching rules.
