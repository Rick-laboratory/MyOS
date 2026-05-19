myOS v101.2 - menu checkmark glyph fix

BUILD: myos_v101_2_menu_checkmark_glyph_fix

Fix on top of v101.1:
- Checked HMENU popup items no longer draw a UTF-8 checkmark through font_draw_str.
- The built-in font is ASCII-only, so the old UTF-8 mark rendered as question marks.
- The check mark is now drawn as compositor/menu chrome in the dedicated mark column, keeping the item text clean.

Build:
  make clean && make

Test:
  DialogLab -> Menu APIs Probe -> View
  The checked item should show a real menu check mark, not ?? before the label.
