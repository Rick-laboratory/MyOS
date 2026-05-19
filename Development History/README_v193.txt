BUILD: myos_v193_modal_owner_activation_focus_stack

v193 hardens DialogBox/modal owner lifetime:

- DialogBox modal loops now push a modal owner/focus/capture state record.
- The owner receives WM_ACTIVATE inactive before disable; the dialog receives activate/focus.
- EndDialog no longer tries to focus a disabled owner in the modal path.
- Closing a nested modal only reenables/restores the modal level it owns.
- Closing the outer modal restores the previous focus/capture target when still valid.
- Nested modal smoke covers owner disabled chain, stale capture release, inner result isolation, and final focus/capture restoration.

This follows the v190-v192 modal pump work and makes DialogBox owner/focus behavior closer to Win32 semantics instead of a simple disable-owner/focus-parent shortcut.
