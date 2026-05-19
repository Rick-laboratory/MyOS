BUILD: myos_v198_resource_template_handle_model

v198 adds a real resource/template handle model instead of treating HINSTANCE/HGLOBAL as raw pointers.

Highlights:
- HRSRC typedef and Win32-style resource APIs: FindResourceA, LoadResource, LockResource, SizeofResource.
- RT_DIALOG / MAKEINTRESOURCEA / IS_INTRESOURCE SDK surface.
- Private registration bridge MyWinRegisterResourceA / MyWinRegisterDialogTemplateResourceA for current static/lab templates and future PE .rsrc loading.
- CreateDialogParamA / DialogBoxParamA now resolve HINSTANCE + RT_DIALOG resources in addition to the legacy lab RegisterDialogTemplateA table.
- GetOpenFileNameA/GetSaveFileNameA support OFN_ENABLETEMPLATE and OFN_ENABLETEMPLATEHANDLE through the real handle model.
- ChooseFontA supports CF_ENABLETEMPLATEHANDLE through HGLOBAL templates.
- No pointer-cast fallback: template handles are locked through LockResource or GlobalLock.
