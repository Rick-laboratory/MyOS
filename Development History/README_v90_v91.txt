BUILD: myos_v90_v91_binary_dlgtemplate_dialog_helpers

v90:
- DLGTEMPLATE/DLGITEMTEMPLATE binary walker remains the central DialogBoxIndirectParamA/CreateDialogIndirectParamA path.
- DialogLab template now includes DS_SETFONT and uses Dialog Units instead of pixel coordinates.
- Dialog units are mapped through MapDialogRect-compatible MulDiv rules:
  x/cx => MulDiv(value, baseX, 4), y/cy => MulDiv(value, baseY, 8).
- Font metrics are intentionally metrics-lite for now: DS_SETFONT is parsed and folded into deterministic base values until the GDI font subsystem exposes real GetTextMetrics.
- DefDlgProcA is now the actual #32770 class WndProc. The app DLGPROC remains a callback with TRUE/FALSE handled semantics.
- DialogLab adds a Dump Template button to inspect the template/resource path before future PE .rsrc loading.

v91:
- Added dialog helper APIs: GetDlgItemInt, SetDlgItemInt, CheckRadioButton, GetNextDlgGroupItem.
- Existing helpers retained: GetDlgItem, SetDlgItemTextA, GetDlgItemTextA, SendDlgItemMessageA, CheckDlgButton, IsDlgButtonChecked, GetNextDlgTabItem.

Known limitation:
- Real Windows derives dialog-unit base metrics from the selected dialog font via GDI GetTextMetrics. myOS still uses a deterministic approximation because the current bitmap-font GDI layer has no full font metric model yet.
