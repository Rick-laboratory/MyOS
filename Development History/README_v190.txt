myOS v190 - real TrackPopupMenu modal loop

BUILD: myos_v190_real_trackpopupmenu_modal_loop

Highlights:
- Removed the old TrackPopupMenu no-input first-item auto-commit from USER32.
- TrackPopupMenu/TrackPopupMenuEx now wait on the current UI queue via MsgWaitForMultipleObjects.
- Queued keyboard/mouse input commits or cancels the menu; no input cancels without WM_COMMAND.
- TPM_RETURNCMD returns the selected command without dispatch.
- TPM_NONOTIFY succeeds without WM_COMMAND.
- OOP child TrackPopupMenu no longer blindly posts the first item; it runs a small child-side modal queue pump and reports commit/cancel over IPC.
- ClipMenuLab status updated from lite/first-item wording to modal behavior.
