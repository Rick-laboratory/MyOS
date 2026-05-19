myOS v75.1 - SurfaceLab see-through / double-WM_CREATE fix

Fixes after v75 runtime screenshot:

1) SurfaceLab no longer looks transparent before the surface is mapped.
   - If no surface is mapped, the child GDI overlay fills the full client area.
   - If a surface is mapped, only the toolbar/status strips are filled so the
     persistent pixel surface stays visible underneath.

2) SurfaceLab state is no longer reset by a second WM_CREATE.
   - v75 bootstrapped the child WndProc manually after CreateWindowExA.
   - A queued/late WM_CREATE could reset g_ChildSurface after the auto Gradient
     draw, leaving mapped=NO/frame=0 and making the client look like a hollow
     overlay.
   - v75.1 initializes SurfaceLab once and ignores duplicate WM_CREATE while
     preserving the mapped surface and counters.

Test:
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

  Right click -> SurfaceLab

Expected:
  - The client background is solid, not see-through.
  - Initial line should show v75.1.
  - Click Map or Gradient.
  - mapped=YES, frame/seq/draws/maps increase.
  - Gradient/Boxes/Clear visibly change the persistent pixel area.
  - Cover/uncover the window: pixels should remain, because the parent blits
    the Surface Section, not stale framebuffer contents.

If Map still fails, report the yellow status line at the bottom; it now survives
paints and duplicate WM_CREATE should not erase it.
