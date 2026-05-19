BUILD: myos_v195_dialog_template_engine_compliance

v195 hardens the USER32 dialog-template path after the v190-v194 dialog pump,
modal owner, and modeless registry work.

Highlights:
- DLGTEMPLATE parser now carries menu metadata and dialog owner relationships.
- DLGTEMPLATEEX parsing keeps helpID/font extended header data and exStyle.
- DS_SETFONT / DS_SHELLFONT feed deterministic DLU->pixel mapping.
- CreateDialog/DialogBox top-level dialogs now receive hWndParent as GW_OWNER
  rather than being unowned top-level windows.
- Added dialogtpl smoke coverage for old templates, extended templates,
  ordinal builtin control classes, tab order, IDs/titles/styles, and DLU scaling.
