BUILD: myos_v137_mdi_command_origin_caption_drag_contract

v137 locks down the MDILab command-origin and MDI caption-drag contract on top
of the v136 coordinate-space fix:

- App menubar raw-input dispatch now distinguishes two real HMENU shapes:
  - popup top-level item: opens the real submenu path
  - direct top-level command: sends exactly one WM_COMMAND to the owner HWND
- This makes MDILab's upper "New child / Tile / Cascade" menubar clickable
  without adding fake hand-painted hit boxes.
- MDILab status logging now records WM_COMMAND id, origin, notification code,
  sender HWND, and sequence number so menu/toolbar/control paths can be told
  apart during manual runs.
- DefMDIChildProcA now has an explicit WM_NCHITTEST -> HTCAPTION path for the
  drawn MDI child caption band.
- Caption drag starts from the canonical negative client-Y band and also accepts
  the stale positive outer-caption coordinate band as the same visual caption
  contract, preventing cascade-mode drag misses.
- MDI drag remains bounded to the MDICLIENT and keeps v131/v133 access-control
  and shell-broker owner-context rules intact.

Smoke additions:
- menu: WindowManager raw menubar direct-command tripwire; one raw click must
  produce exactly one menu-origin WM_COMMAND.
- mdi: cascade-mode caption hit-test, cascade caption drag, positive-Y
  compatibility drag, and existing MDI route tests preserved.

Build/test:
- make clean && make -j2
- ./myos_input --smoke all
