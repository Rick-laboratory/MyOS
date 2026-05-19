myos_v93_1_button_family_msdn_polish

Build on v93 BUTTON family:
- Radio buttons render as round radio glyphs instead of checkbox-like boxes.
- AUTO3STATE indeterminate renders as a filled/solid state.
- BS_DEFPUSHBUTTON has a stronger default-button border.
- Button text rendering strips '&' mnemonics and underlines the mnemonic char.
- Basic Alt+mnemonic dialog routing via IsDialogMessageA/WM_SYSKEYDOWN.
- BS_LEFT/BS_RIGHT/BS_CENTER and BS_TOP/BS_BOTTOM/BS_VCENTER are honored for BUTTON text.
- BS_MULTILINE has a tiny two-line test convention using '|' as a split marker.
- WM_ENABLE/BM_SETCHECK/BM_SETSTATE/BM_SETSTYLE trigger invalidation/repaint.

Known remaining gaps:
- Full Win32 ownerdraw BUTTON semantics are still not implemented.
- Full Visual Styles/UxTheme metrics are out of scope for current framebuffer renderer.
