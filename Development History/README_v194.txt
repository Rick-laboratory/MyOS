BUILD: myos_v194_modeless_dialog_manager

v194 adds the modeless side of the dialog stack:
- CreateDialog*/CreateDialogIndirect* register modeless #32770 HWNDs in the USER32 dialog registry.
- DestroyWindow/EndDialog unregister modeless dialogs immediately so registry slots do not leak.
- A shared modeless dialog pump helper resolves MSG targets back to their owning modeless dialog and routes Tab/Enter/Escape/mnemonics through IsDialogMessageA.
- Modeless dialogs keep their owner enabled; modal owner disable/restore remains handled by the v193 ModalState stack.
- Diagnostics expose modeless register/unregister/live/pump hit counters for smokes.
