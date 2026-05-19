BUILD: myos_v153_user32_dialog_dwlp_modal_v1

v153 USER32 Dialog DWLP/Modal v1

- Adds SDK DWLP_MSGRESULT, DWLP_DLGPROC, DWLP_USER and DLGWINDOWEXTRA constants.
- Registers #32770 with DLGWINDOWEXTRA-sized cbWndExtra storage.
- Keeps GWLP_WNDPROC as DefDlgProcA while storing the app DialogProc in DWLP_DLGPROC.
- DefDlgProcA now treats DialogProc TRUE/FALSE as handled/unhandled and returns DWLP_MSGRESULT for handled messages.
- CreateDialog/DialogBox initialize DWLP slots before WM_INITDIALOG.
- SetWindowLongPtrA(DWLP_DLGPROC) replaces the app dialog callback without subclassing the HWND.
- Modeless EndDialog/direct DestroyWindow cleanup no longer leaks DialogInfo slots.
- Adds smoke group: ./myos_input --smoke user32_dialog_dwlp.
