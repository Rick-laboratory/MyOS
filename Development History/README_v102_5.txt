myOS v102.5 - Child WS_BORDER / Caption Origin Fix

BUILD: myos_v102_5_child_border_caption_origin_fix

Fix:
- mywin_nc_client_y_offset() now tests WS_CAPTION as a full composite style:
  (style & WS_CAPTION) == WS_CAPTION.
- This prevents ordinary WS_BORDER child controls (especially COMBOBOX) from
  being treated as captioned child windows.
- Dropped COMBOBOX mouse capture now gets correct ScreenToClient() coordinates,
  so the visible dropdown rows and click hit-test rows line up exactly.

Validation target:
- DialogLab -> GetOpenFileNameA -> Files of type dropdown.
- Mouse-click the visually highlighted row directly; no one-row/down-offset.
