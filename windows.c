// v118: public WinSDK umbrella translation unit.
//
// windows.h is the umbrella header, but real Win32/MSDN entrypoints are being
// moved into their owning pendant translation units:
//   winuser.h  -> winuser.c
//   winbase.h  -> winbase.c
//   wingdi.h   -> wingdi.c
//   commdlg.h  -> commdlg.c
//   winsvc.h   -> winsvc.c
//   shellapi.h -> shellapi.c
//
// This file intentionally exports no symbols in v118. It exists so the source
// tree mirrors the SDK include surface and old build scripts can discover a
// windows.c pendant for windows.h.
