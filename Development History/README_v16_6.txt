myOS v16.6 - Layout + Terminal + Spy Fixes

Fixes:
- Desktop GRID mode auto-arranges icons on startup/reload.
- Grid cell width increased so labels/icons do not overlap.
- GRID ignores stale .myos_layout collisions; FREE mode still uses saved positions.
- Spy++ detail/status text stays inside the detail box.
- Terminal soft-wrap is word-aware instead of hard cutting at the edge.
- Shell commands receive COLUMNS=<visible terminal cols>.
