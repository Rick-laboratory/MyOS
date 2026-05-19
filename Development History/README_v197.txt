BUILD: myos_v197_commdlg_hook_notify_multiselect_compliance

v197 hardens COMDLG32 file-dialog protocol on top of the v196 real #32770 runtime.

Highlights:
- Adds SDK definitions for NMHDR, OFNOTIFYA/OFNOTIFYEXA, CDN_* notifications and CDM_* messages.
- GetOpenFileNameA/GetSaveFileNameA hooks now receive WM_NOTIFY/CDN_INITDONE and CDN_FILEOK.
- CDN_FILEOK can veto OK and keep the dialog alive.
- CDM_GETSPEC, CDM_GETFILEPATH, CDM_GETFOLDERPATH, CDM_SETCONTROLTEXT, CDM_HIDECONTROL and CDM_SETDEFEXT are handled by the common file dialog proc.
- OFN_ALLOWMULTISELECT writes the documented directory\0file1\0file2\0\0 buffer shape for multi-name selections.
- Template flags remain deterministic CDERR_NOTEMPLATE until myOS has a real resource/template-handle model; the old silent-ignore behavior stays forbidden.
