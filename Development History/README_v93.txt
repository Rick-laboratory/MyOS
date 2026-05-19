myos_v93_button_family_groupbox

v93 completes the classic Win32 BUTTON family on top of the v92.4 evdev/common-control build.

Highlights:
- BUTTON is now type-aware via BS_TYPEMASK.
- Implemented BS_GROUPBOX rendering as a BUTTON-family control.
- GroupBox is non-focusable and non-clickable, matching its role as a frame/caption control.
- Implemented BS_CHECKBOX, BS_AUTOCHECKBOX, BS_3STATE, BS_AUTO3STATE.
- Implemented BS_RADIOBUTTON and BS_AUTORADIOBUTTON, including automatic radio-group clearing using WS_GROUP boundaries.
- BM_CLICK now runs auto-toggle behavior before BN_CLICKED for auto buttons.
- BM_GETCHECK/BM_SETCHECK work for checkbox/radio styles.
- BM_GETSTATE/BM_SETSTATE preserve pushed/focus feedback.
- DialogLab adds "Open Button Dialog" using a DLGTEMPLATE resource with BS_GROUPBOX, AutoCheckBox, Auto3State, AutoRadioButton, Push, OK, Cancel.

Test:
make clean && make
sudo chvt 3
sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

Open:
Start -> DialogLab -> Open Button Dialog

Expected:
- GroupBox draws as a titled frame and does not take focus.
- Tab skips GroupBox and reaches checkbox/radio/push buttons.
- Space toggles AutoCheckBox / Auto3State / AutoRadioButton.
- Mouse click toggles auto buttons.
- AutoRadioButton clears other radios in the same WS_GROUP group.
- OK returns the current button state summary.
