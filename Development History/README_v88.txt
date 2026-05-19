myOS v88 - dialog box modal message loop
========================================

Build tag:
  myos_v88_1_dialog_input_hitfix

What is new
-----------
- Adds Win32-style dialog API surface:
  - DialogBoxParamA(...)
  - CreateDialogParamA(...)
  - MyDialogBoxParamA(...)
  - EndDialog(...)
  - DLGPROC callback typedef
  - WM_INITDIALOG
  - IDOK / IDCANCEL

- Adds a v88 pragmatic built-in dialog template path.
  Resource templates are intentionally not implemented yet.  The built-in
  template creates:
    STATIC  "Name:"
    EDIT    id 101
    BUTTON  IDOK
    BUTTON  IDCANCEL

- Adds DialogLab app:
  - Open Modal Dialog
  - Open Modeless Dialog
  - Last result display: IDOK / IDCANCEL
  - Last text display from the EDIT control

- Modal DialogBoxParamA semantics:
  - disables the parent via EnableWindow(parent, FALSE)
  - runs a private GetMessage/IsDialogMessage/TranslateMessage/DispatchMessage loop
  - EndDialog stores the modal result
  - re-enables the parent and restores focus after the loop returns

- Dialog keyboard behavior:
  - Tab moves between tabstop controls
  - Enter routes to IDOK / default button
  - Esc routes to IDCANCEL
  - OK/Cancel buttons emit WM_COMMAND and call EndDialog through DialogProc

- Input routing fix:
  - keyboard and WM_CHAR routing now respects focused child HWNDs, so EDIT controls
    inside dialogs can receive typed characters instead of the parent consuming them.

Important v88 limits
--------------------
- This is not resource-template support yet.  v89 should add a resource-lite dialog
  template registry/loader.
- The #32770 dialog is implemented as an owned/child HWND within the current myOS
  window-manager frame.  It already behaves modal/modeless for the v88 lab, but it
  is not yet a complete independent top-level owner-window implementation.
- DialogBoxParamA in DialogLab is started from a worker thread because the current
  render/dispatch architecture would otherwise block the visual loop while the modal
  WndProc is inside its own message loop.  The API itself still runs the intended
  private modal message loop.
- OOP child import exposure has the names listed, but the dialog machinery is still
  primarily the parent/in-process USER32-lite path for v88.

How to build
------------
  make clean && make

How to test
-----------
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Then open Start -> DialogLab.

Test checklist
--------------
- Open Modal Dialog.
- Parent should not react while modal dialog is active.
- EDIT starts with "test" from WM_INITDIALOG + SetDlgItemTextA.
- Type into EDIT.
- Tab cycles through EDIT / OK / Cancel.
- Enter returns IDOK.
- Esc returns IDCANCEL.
- OK/Cancel by mouse return IDOK/IDCANCEL.
- Open Modeless Dialog.
- Parent remains usable while modeless dialog is open.
