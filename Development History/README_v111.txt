myOS v111 - MSDN pendant C layout split pass

Scope:
- v109: WinBase/KERNEL32 public implementation cluster moved to winbase.c
  - GetLastError / SetLastError
  - process/env/loader helpers
  - named sections/events/mutexes/semaphores/timers
  - WaitForSingleObject / WaitForMultipleObjects
  - OpenProcess / CreateProcessA / DuplicateHandle / CloseHandle / ExitProcess
- v110: GDI32 public implementation cluster moved to wingdi.c
  - InvalidateRect / ValidateRect
  - BeginPaint / EndPaint / GetDC / ReleaseDC
  - CreateSolidBrush / DeleteObject / SelectObject
  - FillRect / Rectangle / TextOutA / DrawTextA
  - MyGdi* diagnostic/render helpers
- v111: COMDLG32 public implementation cluster moved to commdlg.c
  - CommDlgExtendedError
  - GetOpenFileNameA / GetSaveFileNameA
  - ChooseFontA

Intent:
- Keep the MSDN path obvious: header -> same-named .c file.
- Preserve behavior through smoke tests while moving implementation ownership.
- Leave USER32-only code in winuser.c: HWND/class/message/dialog/control/menu/clipboard/accelerator.
- Keep legacy files as empty compatibility translation units for now.

Build/smoke performed:
- make clean && make
- ./myos_input --smoke all
- Result: PASS, 0 failures

Known WARNs intentionally preserved from v104/v108 baseline:
- CloseHandle(NULL) LastError edge path still incomplete.
- WaitForSingleObject(NULL) LastError edge path still incomplete.
- standalone CreateWindowExA HWND has no compositor WindowState for GetWindowRect.
- MoveWindow/SetWindowPos require WindowManager-backed top-level slot.
