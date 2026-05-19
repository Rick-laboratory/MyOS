myOS v39.2 - ServiceLab click fix + first refactor pass

Fixes:
- ServiceLab button hit-testing now uses the same 8px client inset as rendering.
  v39.1 painted controls at client+8 but tested raw client coords, so visible
  button regions could miss and feel like clicks were being swallowed.
- ServiceLab buttons have a tiny 2px hit slop to make framebuffer/UI clicking
  less pixel-perfect and more desktop-like.
- Object Manager now rejects same-handle / different-type registration instead
  of silently refcounting the wrong object type.

Refactor pass:
- Added app_util.c / app_util.h.
- Removed 12 identical per-app draw_clip_text() implementations and replaced
  them with shared app_draw_clip_text().

Notes on Opus review:
- Confirmed: draw_clip_text duplication was real.
- Confirmed: wm_add_* duplication is real, but not changed yet to avoid mixing
  a window-creation rewrite into this input bugfix.
- Confirmed: _ObjectRegister needed a type collision guard; implemented.
- Confirmed: remove_slot is O(n). It is functionally OK for this queue size,
  but should be changed later together with queue diagnostics/tombstones.
- Confirmed: PID/TID/CapId are conceptually mixed in this PoC. Leave as a
  typed-ID refactor target, not a rushed patch.

Build:
  make clean && make
Run:
  sudo ./myos_input /dev/input/eventX /dev/input/eventY
