myOS v64 - First real out-of-process Editor port

BUILD: myos_v64_1_oop_editor_gdi_textcopy_fix

v64 moves the Texteditor/Notepad image onto the same real GUI-child path that
Calculator proved in v62/v63:

  ShellExecute/MyAppHostLaunch("editor")
    -> gui-ipc subsystem
    -> Process-Lite + THREAD-lite
    -> fork/exec ./myos_apphost_child --gui editor
    -> child WinMain/WndProc/text buffer/caret/save state
    -> RegisterClassExA/CreateWindowExA over ProcessHost IPC
    -> parent Session/WindowManager creates the HWND/frame
    -> WM_KEYDOWN/WM_CHAR/WM_LBUTTONDOWN/WM_MOUSEWHEEL/WM_PAINT over IPC
    -> child writes a generic GDI command buffer into the shared section
    -> parent renders only GDI commands

Editor aliases now using gui-ipc:
  editor
  editor.exe
  notepad
  notepad.exe
  texteditor

New v64 editor shared diagnostics:
  editor_enabled
  editor_revision
  editor_chars_typed
  editor_keydowns
  editor_cursor/editor_length/editor_dirty/editor_scroll_line
  editor_path/editor_name/editor_status/editor_preview

Keyboard path:
  main.c now posts WM_KEYDOWN and, for printable keys, WM_CHAR to the focused
  HWND. The out-of-process editor uses WM_KEYDOWN for navigation/backspace/save
  and WM_CHAR for normal text input.

Regression kept:
  Calculator remains out-of-process and still renders via the generic GDI IPC
  command buffer.

Smoke tests performed:
  editor child:
    CreateWindowExA over IPC
    initial GDI buffer
    WM_CHAR 'H' + 'i'
    editor preview == "Hi"
    F10 save writes /tmp/myos_v64_editor_smoke.txt
    WM_CLOSE exits child with code 64

  calculator regression:
    CreateWindowExA over IPC
    initial GDI buffer
    WM_LBUTTONDOWN on button 7
    display == "7"
    WM_CLOSE exits child with code 63
