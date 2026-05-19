BUILD: myos_v155_user32_gdi_redraw_updatewindow_v1

v155 USER32/GDI Redraw + UpdateWindow v1

Focus:
- GetUpdateRect
- UpdateWindow
- RedrawWindow
- RDW_* public SDK constants
- update-region dirty/coalescing hardening
- ValidateRect / BeginPaint remove stale queued WM_PAINT intents
- RedrawWindow RDW_ALLCHILDREN / RDW_NOCHILDREN propagation

Smoke:
- ./myos_input --smoke user32_redraw -> PASS 34/34
- ./myos_input --smoke all -> PASS 815 PASS, 0 FAIL, 0 WARN

Notes:
- This is still rect-based GDI-lite, not HRGN/complex region GDI.
- WM_PAINT remains coalesced. v155 adds explicit update-region inspection
  and synchronous paint entrypoints without introducing paint floods.
