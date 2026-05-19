myOS v170 - remove boot-time demo hooks

BUILD: myos_v170_remove_demo_hooks

Purpose
-------
v169 proved that retained OOP GDI command streams are HWND-targeted. The old
boot-time hook demo was still installed in normal desktop runtime and produced
console spam such as:

    [HOOK] WM_KEYDOWN hwnd=... key=...

That hook was useful as an early proof that the hook chain could observe and
block messages, but it should not be part of the normal desktop path anymore.

Changes
-------
1. Removed the runtime-installed `log_hook` demo from `main.c`.
2. Removed the runtime-installed F1 blocker demo from `main.c`.
3. Kept the HWND hook chain/API intact in `hwnd.c` / `hwnd.h`.
4. Kept v169 per-HWND OOP GDI stream architecture unchanged.

Result
------
Normal desktop keyboard input no longer logs `[HOOK] WM_KEYDOWN ...` for every
key press, and F1 is no longer blocked by the old demo hook.
