# myOS v220 - Internal underscore nomenclature boundary cleanup

v220 keeps public Win32/MSDN-facing symbols public and moves the new internal
Object/Header/HWND state nomenclature away from project-prefix names.

Rule applied in this version:

- Public SDK/MSDN names remain unchanged: HANDLE, HWND, SECURITY_DESCRIPTOR,
  ACL, WM_*, WS_*, SW_*, GENERIC_MAPPING, TOKEN_USER, etc.
- Internal Object Manager names use `_Object*` / `_OBJECT_*`.
- Internal USER/HWND header/state names use `_Hwnd*` / `_HWND_*`.

This is a naming/boundary cleanup only; it does not change the v218/v219
slot/generation/state semantics.

Validated:
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all
