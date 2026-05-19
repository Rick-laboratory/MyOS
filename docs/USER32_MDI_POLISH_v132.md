# v132 USER32 MDI Polish Contract

v132 builds on:

- v129: MDICLIENT, MDI child ownership, command routing
- v130: Parent / Owner / Thread-Affinity split
- v131: HWND access-control and MDI injection guards

The goal is to make MDI useful enough to test manually and stable enough to
protect future polish work.

## Implemented in USER32

### Layout messages

- `WM_MDITILE`
  - Collects current MDI children of the MDICLIENT.
  - Places them in a deterministic tile layout inside the MDICLIENT rectangle.
  - `MDITILE_HORIZONTAL` is supported as a simple horizontal pass.

- `WM_MDICASCADE`
  - Places children in a deterministic stepped cascade.
  - Keeps the active child/focus model from the v129/v130 work.

- `WM_MDIICONARRANGE`
  - Safe no-op for now.
  - Returns success so callers can use the message without corrupting state.

### Window menu / menu merge lite

- `WM_MDISETMENU` now stores both:
  - frame menu
  - window menu

- MDICLIENT refreshes the configured window menu with live MDI children using
  `idFirstChild + index` command IDs.

- Creating or destroying MDI children refreshes the window menu list.

This is intentionally a lite window-list contract, not full Windows menu merge.
It is enough for stable command IDs and visible child-list behavior.

## New visible MDILab

v132 adds a classic parent-side `mdi-lab` app:

- Start menu entry: `MDILab`
- AppHost aliases:
  - `mdi-lab`
  - `mdilab`
- App type: `APP_MDILAB`

The lab creates:

- a top-level `myOS.MDILab` frame
- a `MDICLIENT` child
- multiple `myOS.MDILabChild` MDI children
- buttons for New, Tile, Cascade, Next and Close

The lab is a canary. It should make MDI behavior visible without becoming a
private-myOS-hack application.

## Smoke coverage

The `mdi` group now gates:

- multiple MDI children
- child ID assignment from `CLIENTCREATESTRUCT::idFirstChild`
- MDI child activation
- `WM_MDINEXT`
- `DefFrameProcA` command routing
- `WM_MDISETMENU`
- window-menu child list refresh
- `WM_MDICASCADE`
- `WM_MDITILE`
- `WM_MDIICONARRANGE`
- `WM_MDIDESTROY`
- child teardown through `DestroyWindow(MDICLIENT)`

The `app_labs` group now includes `mdi-lab` as a visible App/Lab canary.

## Still open

Not implemented yet:

- full MDI Window menu merge behavior
- minimized/iconic MDI child rendering
- full MDI maximize/restore frame-menu merge
- MDI client scrollbars
- Tile/cascade parity with every Windows edge case

Those should come only after the existing tripwires remain stable.
