myOS v102.1 - Common Dialogs modal paint fix

Fixes the v102 issue where GetOpenFileNameA/GetSaveFileNameA created the
modal #32770 dialog and then appeared to freeze DialogLab until Escape.

Root cause:
- In-process myOS test apps currently share the render/compositor thread.
- DialogBoxIndirectParamA entered a blocking GetMessageA modal loop from inside
  hwnd_dispatch().
- The dialog existed and Escape could cancel it, but the compositor never got
  a chance to call MyDrawTopLevelDialogs(), so the UI looked stuck.

Fix:
- Added an internal modal idle callback, wired by main.c.
- DialogBoxIndirectParamA now uses a short timed queue wait and runs the
  compositor idle/redraw hook between modal messages.
- The current dialog queue is still marked external_pump=1, so the global
  hwnd_dispatch() does not recursively consume modal dialog messages.

Result:
- GetOpenFileNameA/GetSaveFileNameA display immediately.
- Mouse, Tab, arrows, Enter and Escape remain routed through the modal loop.
- Owner disabling/re-enabling stays intact.

Build:
    make clean && make

Run:
    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

Test:
- DialogLab -> GetOpenFileNameA
- DialogLab -> GetSaveFileNameA
- Verify the dialog becomes visible immediately instead of only cancelling on Escape.
