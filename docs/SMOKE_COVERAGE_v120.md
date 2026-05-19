# myOS v120 Smoke Coverage Tripwires

v120 broadens the smoke runner from a narrow build gate into a refactor tripwire.
This is still not a formal Win32 conformance suite. The purpose is to make the
high-value Lab axes testable without manual click-through every time USER32,
KERNEL32, GDI, menu, capture, or section internals move.

## New smoke groups

### gdi
Covers:
- `InvalidateRect`
- dirty rect preservation through `MyGdiGetWindowState`
- `BeginPaint` / `EndPaint`
- paint DC lifetime
- `CreateSolidBrush`
- `SelectObject` selected-count bookkeeping
- `FillRect`
- `Rectangle`
- `TextOutA`
- `GetDC` / `ReleaseDC`
- `ValidateRect`

### menu
Covers:
- `CreateMenu`
- `CreatePopupMenu`
- `AppendMenuA`
- submenu attachment
- `GetMenuItemCount`
- `GetSubMenu`
- `GetMenuItemID`
- `GetMenuItemInfoA`
- `CheckMenuItem`
- `EnableMenuItem`
- `SetMenu` / `GetMenu`
- `DrawMenuBar`
- `TrackPopupMenu(TPM_RETURNCMD)`
- `TrackPopupMenu` owner `WM_COMMAND` dispatch
- `CreateAcceleratorTableA`
- `TranslateAcceleratorA`
- `DestroyAcceleratorTable`

### capture
Covers:
- `SetCapture`
- capture replacement
- `WM_CAPTURECHANGED` on replacement
- `ReleaseCapture`
- `WM_CAPTURECHANGED` on release
- invalid/no-owner capture release paths

### ipc_section
Covers:
- named `CreateFileMappingA`
- second-handle `OpenFileMappingA`
- two `MapViewOfFile` views over the same section
- payload visibility between views
- named event signal roundtrip through `CreateEventA` / `OpenEventA`
- `ResetEvent`
- `SetEvent`
- `WaitForSingleObject`
- `FlushViewOfFile`

### handle_invalid
Covers:
- `CloseHandle(NULL)`
- `CloseHandle(INVALID_HANDLE_VALUE)`
- double close
- `DuplicateHandle` invalid source
- `DuplicateHandle` unknown source
- `DuplicateHandle` null target pointer
- expected `GetLastError` values for each path

### wait_invalid
Covers:
- `WaitForSingleObject(NULL)`
- `WaitForSingleObject(INVALID_HANDLE_VALUE)`
- `WaitForMultipleObjects` count zero
- `WaitForMultipleObjects` null handle array
- `WaitForMultipleObjects` invalid handle in array
- valid single-object wait sanity check

### last_error
Covers:
- `SetLastError` / `GetLastError` sentinel roundtrip
- missing `OpenEventA`
- missing `OpenFileMappingA`
- zero-sized `CreateFileMappingA`
- `MapViewOfFile(NULL)`

## Core behavior hardened in v120

The following paths now set smoke-gated `GetLastError` values:

- `CloseHandle`: invalid handle -> `ERROR_INVALID_HANDLE`
- `DuplicateHandle`: null target pointer -> `ERROR_INVALID_PARAMETER`
- `DuplicateHandle`: invalid source -> `ERROR_INVALID_HANDLE`
- `WaitForSingleObject`: invalid handle -> `ERROR_INVALID_HANDLE`
- `WaitForMultipleObjects`: invalid parameter -> `ERROR_INVALID_PARAMETER`
- `WaitForMultipleObjects`: invalid handle -> `ERROR_INVALID_HANDLE`
- `OpenEventA`: missing object -> `ERROR_FILE_NOT_FOUND`
- `OpenFileMappingA`: missing object -> `ERROR_FILE_NOT_FOUND`
- `CreateFileMappingA`: zero/oversized mapping -> `ERROR_INVALID_PARAMETER`
- `MapViewOfFile`: invalid section handle -> `ERROR_INVALID_HANDLE`

## Known WARNs intentionally kept

The `user32` group still warns for `GetWindowRect` and `MoveWindow` on standalone
`CreateWindowExA` windows without a compositor-backed `WindowState`. That gap is
real, but v120 keeps it visible rather than papering it over. It belongs to the
upcoming USER32/window/focus/capture contract pass.
