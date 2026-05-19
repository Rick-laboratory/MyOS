# myOS / Linux-Win32 v243 — WindowInfo Hot/Cold Control-Payload Pass

v243 focuses on the next cacheline-quality step after the v239-v242 O(1) and
queue/hotpath passes: keeping USER32's frequently traversed WindowInfo array
small even when a window owns heavy control, combo/listbox, text, and MDI state.

## Main changes

- MyWinWindowInfo now keeps the HWND metadata, parent/owner links, WndProc,
  style/exStyle, z-order, rcClient, userData, and cbWndExtra pointers in the
  compact hot/warm record.
- Bulky window text/class strings and classic control payloads are out-of-line:
  - className buffer
  - window text buffer
  - LISTBOX/COMBOBOX item text table
  - item-data table
  - multi-select bitmap
- BUTTON/EDIT/LISTBOX/COMBOBOX/SCROLLBAR state is grouped in
  MyWinWindowControlState.
- MDI-client/MDI-child bookkeeping is grouped in MyWinWindowMdiState.
- Window creation allocates the cold buffers once; hot lookup/dispatch paths do
  not allocate.
- DestroyWindow releases the out-of-line buffers before resetting the slot.
- Control item shifting uses memmove for fixed-width adjacent item buffers,
  removing GCC overlap warnings after pointer-backing the item table.
- Smoke now logs the WindowInfo stride and out-of-line cold bytes:
  WindowInfo_stride is bounded below eight cachelines while ~14 KiB of payload
  is kept out of the hot slot array.

## Validation

- `make clean && make -j2`
- `./myos_input --smoke all`

The v243 smoke result is PASS with 0 failures.
