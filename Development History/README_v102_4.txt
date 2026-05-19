myOS v102.4 - Common Dialogs Combo Layout/Hit-Test Fix

BUILD: myos_v102_4_common_dialogs_combo_layout_hittest_fix

Focus:
- Fix GetOpenFileNameA/GetSaveFileNameA filter combo layout.
- Give the "Files of type:" label enough room so it is not obscured/truncated by the COMBOBOX.
- Redraw only the dropped COMBOBOX popup in the topmost popup pass, not the closed combo body again.
- Align COMBOBOX mouse row hit-testing with the exact painted popup row geometry.
- Keep the existing OPENFILENAMEA/GetOpenFileNameA/GetSaveFileNameA behavior unchanged.

Test:
  make clean && make
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

DialogLab:
- Open GetOpenFileNameA.
- Open the "Files of type" combo.
- Click each visible filter row with the mouse; the clicked row should be selected.
- The "Files of type:" label should no longer be covered/truncated by the combo.
