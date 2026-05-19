myOS v89 - resource template dialogs
====================================

Build label:
  myos_v89_resource_template_dialogs

Goal:
  Move v88 dialog creation from hardcoded USER32-built controls to MSDN-shaped
  dialog templates. Dynamic CreateWindowExA controls remain supported, but
  dialogs can now be created from DLGTEMPLATE / DLGITEMTEMPLATE data.

New / changed USER32 APIs:
  - DLGTEMPLATE / DLGITEMTEMPLATE public struct names
  - DLGTEMPLATEEX / DLGITEMTEMPLATEEX parser skeleton
  - DialogBoxIndirectParamA(..., LPCDLGTEMPLATEA, ...)
  - CreateDialogIndirectParamA(..., LPCDLGTEMPLATEA, ...)
  - DialogBoxParamA(..., LPCSTR name, ...) now does name lookup -> IndirectParam
  - CreateDialogParamA(..., LPCSTR name, ...) now does name lookup -> IndirectParam
  - RegisterDialogTemplateA(name, template) for myOS resource-registry tests
  - FindDialogTemplateA(name)

Compatibility notes:
  - Old DLGTEMPLATE + DLGITEMTEMPLATE binary layout is supported for string
    and ordinal/atom class names.
  - Built-in dialog class atoms supported:
      0x0080 BUTTON
      0x0081 EDIT
      0x0082 STATIC
      0x0083 LISTBOX      (parsed; control class not implemented yet)
      0x0084 SCROLLBAR    (parsed; control class not implemented yet)
      0x0085 COMBOBOX     (parsed; control class not implemented yet)
  - DLGTEMPLATEEX header and item parsing is present, including HelpID fields.
  - DS_SETFONT/DS_SHELLFONT font data is skipped so the parser lands on items;
    real dialog-unit/font metric conversion is not complete yet.

DialogLab changes:
  - AccessProbeDialog is now a static DLGTEMPLATE resource in app_dialoglab.c.
  - WM_CREATE registers it with RegisterDialogTemplateA("AccessProbeDialog", ...).
  - Open Modal Dialog uses DialogBoxIndirectParamA() directly.
  - Open Modeless Dialog uses CreateDialogParamA(), exercising name -> template lookup.

Expected test:
  make clean && make
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Open Start -> DialogLab:
  - modal dialog opens from DLGTEMPLATE pointer
  - modeless dialog opens from resource-name lookup
  - EDIT, OK, Cancel are created from DLGITEMTEMPLATE entries
  - Tab, typing, Enter=IDOK, Esc=IDCANCEL should still behave like v88.3
  - modeless remains beside parent; modal disables parent

Known remaining compliance work:
  - Dialog units are still treated close to pixels; real font-based MapDialogRect
    behavior is a later MSDN-compliance step.
  - Full PE .rsrc enumeration is not implemented yet; the resource registry is
    the bridge until the PE loader can pass mapped template pointers directly.
