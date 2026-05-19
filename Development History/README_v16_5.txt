myOS v16.5 - Terminal + Spy++ small fixes

Fixes:
- Spy++ text layout: stats/header/rows no longer overlap in normal window sizes.
- Terminal output wraps to the current window width instead of drawing outside the client area.
- Terminal draw path shows the latest visible wrapped lines and clips text to the window.
- Terminal scrollback increased from 24x78 to 256x240 internal chars.
- Long command input stays inside the input bar and scrolls horizontally to keep the cursor visible.

Build:
  make clean && make
  ./myos_input
