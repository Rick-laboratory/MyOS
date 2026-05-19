BUILD: myos_v196_commdlg_dialog_runtime_compliance

v196 moves COMDLG32-style GetOpenFileNameA/GetSaveFileNameA/ChooseFontA onto the hardened v192-v195 dialog runtime.

Compliance focus:
- Common dialogs use real #32770 DialogBoxIndirectParamA runtime, controls and owner modal restore.
- OFN/CF hook flags are validated and hook callbacks are invoked.
- Template flags fail deterministically with CDERR_NOTEMPLATE until resource-template merge support is implemented.
- CommDlgExtendedError reports exact CDERR/FNERR/CFERR validation failures.
- GetOpenFileNameA appends lpstrDefExt before OFN_FILEMUSTEXIST checks and fills title/offset fields.
- ChooseFontA validates CF_USESTYLE, CF_FORCEFONTEXIST and preserves LOGFONT fields for CF_NOFACESEL/CF_NOSIZESEL/CF_NOSTYLESEL.
