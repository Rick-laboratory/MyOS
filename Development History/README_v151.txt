BUILD: myos_v151_user32_windowlong_classlong_v1

v151 turns the old minimal v41 WindowLong storage into real USER32 class/window
metadata storage on top of the v150 per-process class table.

Implemented:

1. Per-HWND WindowLongPtr storage
   - GWLP_USERDATA
   - GWLP_WNDPROC
   - GWLP_HINSTANCE
   - GWLP_HWNDPARENT
   - GWLP_ID
   - GWL_STYLE
   - GWL_EXSTYLE
   - cbWndExtra byte ranges from WNDCLASSEXA::cbWndExtra

2. Per-class ClassLongPtr storage
   - GetClassLongPtrA / SetClassLongPtrA
   - GetClassLongA / SetClassLongA
   - GCL_STYLE
   - GCL_CBCLSEXTRA
   - GCL_CBWNDEXTRA
   - GCLP_WNDPROC
   - GCLP_HMODULE
   - GCLP_HICON / GCLP_HICONSM
   - GCLP_HCURSOR
   - GCLP_HBRBACKGROUND
   - GCLP_MENUNAME
   - GCW_ATOM
   - cbClsExtra byte ranges from WNDCLASSEXA::cbClsExtra

3. Subclassing path
   - SetWindowLongPtrA(GWLP_WNDPROC) replaces the current per-HWND WndProc.
   - The old WndProc is returned.
   - Dispatch/SendMessage use the current per-HWND WndProc.
   - CallWindowProcA remains the explicit chain helper.

4. v150 isolation preserved
   - Class extra storage belongs to the process-owned class entry.
   - Two processes may register the same class name and keep independent
     cbClsExtra storage and class WndProc state.
   - System classes such as BUTTON remain protected from app SetClassLongPtrA
     mutation.

5. Error behavior tightened
   - Invalid HWND LongPtr access returns ERROR_INVALID_WINDOW_HANDLE.
   - cbWndExtra/cbClsExtra out-of-bounds access returns ERROR_INVALID_INDEX.
   - Foreign mutation still returns ERROR_ACCESS_DENIED through the existing
     HWND access-control layer.

Smoke:

  ./myos_input --smoke user32_longptr
  PASS: 35/35

  ./myos_input --smoke all
  PASS: 708, FAIL: 0, WARN: 0

Notes / intentionally still out of scope:

- Dialog DWLP_* aliases are not completed here.
- Full WM_STYLECHANGING / WM_STYLECHANGED and SetWindowPos frame recalculation
  are still future work.
- Cross-process subclassing remains blocked by the existing owner-only mutation
  policy.
