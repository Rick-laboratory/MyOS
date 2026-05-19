myOS v102.3 - Common Dialogs combo dropdown hit-test/z-order fix

Scope:
- Built on v102.2.
- Fixes the GetOpenFileNameA/GetSaveFileNameA filter COMBOBOX dropdown being painted under later sibling controls such as the Read only checkbox.
- Fixes mouse selection in the opened filter dropdown by giving dropped COMBOBOX popup chrome priority in ChildWindowFromPoint hit-testing.
- Keeps the v102.2 closed-height fix: the filter combo remains a normal one-line CBS_DROPDOWNLIST control; only the transient dropdown area expands.
- Sizes the Common Dialog filter dropdown to the actual filter item count, capped at six rows.

Tests:
- make clean && make
- DialogLab -> GetOpenFileNameA
- Open the Files of type dropdown. The dropdown must paint above Read only/status controls, and clicking C Source/Text/All Files must select via mouse.
