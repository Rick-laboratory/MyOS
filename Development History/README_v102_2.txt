myOS v102.2 - Common Dialogs filter combo height fix

Fixes the visual/layout issue in the v102 common file dialogs where the
"Files of type" COMBOBOX used a control height of 60 dialog units.

That made the closed combo body visibly hang downward into the lower area of
the dialog. For a CBS_DROPDOWNLIST-style control, the base control rectangle
should stay at the normal closed height; the drop list itself is rendered only
when the combo is opened.

Changes:
- Common file dialog template: filter COMBOBOX height changed from 60 to 14
  dialog units.
- Existing runtime drop-list height behavior remains unchanged via
  wi->ccDropHeight, so the dropdown still opens with a usable list area.
- Build/runtime labels updated to v102.2.

Validation:
- make clean && make
- Open DialogLab -> GetOpenFileNameA / GetSaveFileNameA
- Verify the filter combo no longer hangs below the dialog when closed.
