BUILD: myos_v157_user32_gdi_erase_background_v1

v157 USER32/GDI Erase Background / Class Brush v1

Highlights:
- WM_ERASEBKGND constant added to SDK.
- BeginPaint now consumes erasePending, exposes PAINTSTRUCT.fErase and sends WM_ERASEBKGND with the paint DC.
- If the erase handler returns FALSE, DefWindowProcA applies the class hbrBackground brush.
- DefWindowProcA(WM_ERASEBKGND) fills the client area using GCLP_HBRBACKGROUND.
- InvalidateRect(TRUE), GetUpdateRect(..., TRUE), RDW_ERASE and RDW_NOERASE now have explicit erasePending semantics.
- RDW_ERASENOW/UPDATENOW use the synchronous UpdateWindow path.
- MyGdiGetWindowState diagnostics now expose erasePending/internalPaint.

Smoke:
- ./myos_input --smoke user32_erase : PASS 33/33
- ./myos_input --smoke user32_redraw : PASS 34/34
- ./myos_input --smoke gdi_bitmap_dc : PASS 30/30
- ./myos_input --smoke all : PASS 878 PASS, 0 FAIL, 0 WARN

Still intentionally out of scope:
- GetUpdateRgn/ValidateRgn and true HRGN clipping.
- WM_CTLCOLOR*, system color brushes and theme brush handling.
- Full non-client erase and DWM dirty-region scheduling.
