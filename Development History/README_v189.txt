myOS v189 - HWND child tree close fix

Fixes the v188 regression visible in the debug badge where HWND count stayed high after all apps were closed.

Root cause: shell close was calling DestroyWindow while USER32 was still bound to the shell/session capability. DestroyWindow correctly rejected foreign owner-thread mutation, and WindowManager fell back to raw hwnd_destroy(). That bypassed USER32 child-tree teardown and leaked child HWNDs such as DialogLab buttons, modal OK/Cancel controls, ControlLab child controls and ServiceLab controls.

Fix: WindowManager now temporarily enters the real window-owner capability before the authoritative DestroyWindow path, so DestroyWindow(parent) recursively destroys children through normal WM_DESTROY/WM_NCDESTROY semantics.

Smoke coverage adds AppHost HWND child-tree cleanup auditing.
