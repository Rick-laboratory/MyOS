BUILD: myos_v154_popup_menu_modal_loop_v1

v154 USER32 Popup Menu Modal Loop v1

- Reworks TrackPopupMenu from a pre-init first-command shortcut into a small modal popup tracker.
- Sends WM_INITMENU/WM_INITMENUPOPUP before resolving the selected item so owner code can enable/disable or rewrite menu contents.
- Adds queued keyboard handling for popup menus: UP/DOWN selection, ENTER/SPACE commit, ESC cancel.
- Adds queued mouse-up row selection for the PoC popup surface model.
- Adds TPM_NONOTIFY behavior so selection can succeed without WM_COMMAND dispatch.
- Adds TrackPopupMenuEx and TPMPARAMS SDK declarations/validation.
- Keeps TPM_RETURNCMD command-return mode separate from WM_COMMAND dispatch.
- Preserves capture/focus restoration and close notifications.
- Adds smoke group: ./myos_input --smoke user32_popup_modal.
