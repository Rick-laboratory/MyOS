# v134 MDILab Render/Input Fix

v132 added the first visible MDILab, and v133 fixed queued shell dispatch under
strict HWND access control. Manual testing then exposed two MDILab-specific
issues:

1. The real BUTTON child HWNDs were created at client y=8 while the compositor
   draws the app `HMENU` bar at the top of the same client area. The menu bar
   therefore overpainted the toolbar.
2. MDI children could be created successfully (`WM_MDICREATE -> hwnd=...`) but
   the manual MDILab blit path could fail to read their geometry when the
   compositor was not already running under the MDILab owner capability.

## Changes

- MDILab now reserves `APP_MENUBAR_H` before the toolbar.
- The toolbar is only real USER32 `BUTTON` controls. The old hand-painted fake
  toolbar buttons were removed to avoid double drawing and input ambiguity.
- `MDICLIENT` is laid out below the toolbar and above the status line.
- `WM_SIZE` relayouts toolbar controls and the MDI client.
- `mdilab_blit()` temporarily enters the MDILab owner process context while
  reading MDI child/client `GetWindowRect()` data, then restores the previous
  context.
- The app_labs smoke child-HWND probe now uses a MDILab-specific point below the
  menu bar.

## Still intentionally not the final MDI layer

This is a Canary/UX repair pass, not the final Windows MDI endgame. Remaining
future work includes minimized MDI child surfaces, full system-menu/maximize
merge behavior, and a richer window-list/menu integration.
