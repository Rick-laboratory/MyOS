myOS v42.1 - Build marker correction

Basis: v42_window_tree_lifetime.

This is a small correctness cleanup only. The v42 code was present, but a few runtime
strings still displayed old v39/v39.2 build names:

- main.c console banner
- main.c top-left debug badge prefix
- window.c visible BUILD marker

No architecture behavior was intentionally changed.

Build/test:
  make clean && make
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Expected visible marker:
  BUILD v42.1: USER32 tree/lifetime
