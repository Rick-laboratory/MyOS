#include "apphost.h"
#include "window.h"
#include "processhost.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>

typedef int (*MyAppWinMainProc)(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow);
typedef int (*MyAppMainProc)(int argc, char** argv);

typedef struct MyImportDescriptor {
    const char* module;
    const char* proc;
} MyImportDescriptor;

typedef struct MyAppEntry {
    const char* image;
    const char* defaultTitle;
    const char* capName;
    DWORD       capFlags;
    MyAppWinMainProc winEntry;
    MyAppMainProc mainEntry;
    const char* entryName;
    const MyImportDescriptor* imports;
    DWORD       importCount;
    const char* subsystem;
} MyAppEntry;

typedef struct MyAppEntryFrame {
    WindowManager* wm;
    int x;
    int y;
    char title[96];
    LPCSTR path;
    Capability cap;
} MyAppEntryFrame;

static MyAppEntryFrame g_AppEntryFrame;

static Capability apphost_entry_cap(void)
{
    const Capability* cur = MyWinGetCurrentCapability();
    if (cur) return *cur;
    return g_AppEntryFrame.cap;
}

static int __attribute__((unused)) app_winmain_calc(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_calc(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "Rechner", apphost_entry_cap()); }

static int __attribute__((unused)) app_winmain_editor(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_editor(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.path, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "Texteditor", apphost_entry_cap()); }

static int app_winmain_spy(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_spy(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS Spy++", apphost_entry_cap()); }

static int app_winmain_access(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_access(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS AccessLab", apphost_entry_cap()); }

static int app_winmain_pump(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_pump(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS PumpLab", apphost_entry_cap()); }

static int app_winmain_deadlock(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_deadlock(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS DeadlockLab", apphost_entry_cap()); }

static int app_winmain_section(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_section(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS SectionLab", apphost_entry_cap()); }

static int app_winmain_object(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_objectlab(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS ObjectLab", apphost_entry_cap()); }

static int app_winmain_waitlab(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_waitlab(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS WaitLab", apphost_entry_cap()); }

static int __attribute__((unused)) app_winmain_clipmenu(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_clipmenulab(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS ClipMenuLab", apphost_entry_cap()); }

static int __attribute__((unused)) app_winmain_paintlab(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_paintlab(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS PaintLab", apphost_entry_cap()); }

static int __attribute__((unused)) app_winmain_draglab(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_draglab(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS DragLab", apphost_entry_cap()); }

static int __attribute__((unused)) app_winmain_controllab(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_controllab(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS ControlLab", apphost_entry_cap()); }

static int app_winmain_dialoglab(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_dialoglab(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS DialogLab", apphost_entry_cap()); }

static int app_winmain_servicelab(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_servicelab(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS ServiceLab", apphost_entry_cap()); }

static int app_winmain_mdilab(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{ (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow; return wm_add_mdilab(g_AppEntryFrame.wm, g_AppEntryFrame.x, g_AppEntryFrame.y, g_AppEntryFrame.title[0] ? g_AppEntryFrame.title : "myOS MDILab", apphost_entry_cap()); }

static int app_main_argdump(int argc, char** argv)
{
    printf("[v61 ipc] argdump main(argc=%d)", argc);
    for (int i = 0; i < argc; i++) printf(" argv[%d]='%s'", i, argv && argv[i] ? argv[i] : "");
    printf("\n");
    return 60;
}

static int app_main_sleeper(int argc, char** argv)
{
    (void)argc; (void)argv;
    return 60;
}

static const MyImportDescriptor g_GuiImports[] = {
    { "kernel32.dll", "GetCommandLineA" },
    { "kernel32.dll", "GetStartupInfoA" },
    { "kernel32.dll", "GetModuleHandleA" },
    { "kernel32.dll", "GetModuleFileNameA" },
    { "kernel32.dll", "GetLastError" },
    { "user32.dll",   "RegisterClassExA" },
    { "user32.dll",   "CreateWindowExA" },
    { "user32.dll",   "RegisterDialogTemplateA" },
    { "user32.dll",   "FindDialogTemplateA" },
    { "user32.dll",   "DialogBoxIndirectParamA" },
    { "user32.dll",   "CreateDialogIndirectParamA" },
    { "user32.dll",   "DialogBoxParamA" },
    { "user32.dll",   "CreateDialogParamA" },
    { "user32.dll",   "EndDialog" },
    { "user32.dll",   "DefWindowProcA" },
    { "user32.dll",   "DestroyWindow" },
    { "user32.dll",   "PostMessageA" },
    { "user32.dll",   "GetMessageA" },
    { "user32.dll",   "SetTimer" },
    { "user32.dll",   "KillTimer" },
    { "user32.dll",   "DispatchMessageA" },
    { "user32.dll",   "InvalidateRect" },
    { "user32.dll",   "ValidateRect" },
    { "user32.dll",   "GetUpdateRect" },
    { "user32.dll",   "UpdateWindow" },
    { "user32.dll",   "RedrawWindow" },
    { "user32.dll",   "InvalidateRgn" },
    { "user32.dll",   "ValidateRgn" },
    { "user32.dll",   "GetUpdateRgn" },
    { "gdi32.dll",    "CreateRectRgn" },
    { "gdi32.dll",    "CreateRectRgnIndirect" },
    { "gdi32.dll",    "SetRectRgn" },
    { "gdi32.dll",    "OffsetRgn" },
    { "gdi32.dll",    "GetRgnBox" },
    { "gdi32.dll",    "CombineRgn" },
    { "gdi32.dll",    "EqualRgn" },
    { "gdi32.dll",    "PtInRegion" },
    { "gdi32.dll",    "RectInRegion" },
    { "gdi32.dll",    "SelectClipRgn" },
    { "gdi32.dll",    "ExcludeClipRect" },
    { "gdi32.dll",    "IntersectClipRect" },
    { "gdi32.dll",    "GetClipBox" },
    { "user32.dll",   "ScrollWindow" },
    { "user32.dll",   "ScrollWindowEx" },
    { "gdi32.dll",    "CreateCompatibleDC" },
    { "gdi32.dll",    "DeleteDC" },
    { "gdi32.dll",    "CreateCompatibleBitmap" },
    { "gdi32.dll",    "CreateBitmap" },
    { "gdi32.dll",    "CreateDIBSection" },
    { "gdi32.dll",    "SelectObject" },
    { "gdi32.dll",    "DeleteObject" },
    { "gdi32.dll",    "GetObjectA" },
    { "gdi32.dll",    "SetPixel" },
    { "gdi32.dll",    "GetPixel" },
    { "gdi32.dll",    "BitBlt" },
    { "gdi32.dll",    "PatBlt" },
    { "gdi32.dll",    "StretchBlt" },
    { "gdi32.dll",    "GetStretchBltMode" },
    { "gdi32.dll",    "SetStretchBltMode" },
    { "gdi32.dll",    "GetDIBits" },
    { "gdi32.dll",    "SetDIBits" },
    { "gdi32.dll",    "StretchDIBits" },
    { "gdi32.dll",    "SetDIBitsToDevice" },
    { "user32.dll",   "SetCapture" },
    { "user32.dll",   "ReleaseCapture" },
    { "user32.dll",   "OpenClipboard" },
    { "user32.dll",   "CloseClipboard" },
    { "user32.dll",   "EmptyClipboard" },
    { "user32.dll",   "SetClipboardData" },
    { "user32.dll",   "GetClipboardData" },
    { "user32.dll",   "IsClipboardFormatAvailable" },
    { "user32.dll",   "CreateMenu" },
    { "user32.dll",   "CreatePopupMenu" },
    { "user32.dll",   "AppendMenuA" },
    { "user32.dll",   "InsertMenuA" },
    { "user32.dll",   "ModifyMenuA" },
    { "user32.dll",   "RemoveMenu" },
    { "user32.dll",   "DeleteMenu" },
    { "user32.dll",   "CheckMenuItem" },
    { "user32.dll",   "EnableMenuItem" },
    { "user32.dll",   "SetMenu" },
    { "user32.dll",   "GetMenu" },
    { "user32.dll",   "DrawMenuBar" },
    { "user32.dll",   "TrackPopupMenu" },
    { "user32.dll",   "TrackPopupMenuEx" },
    { "user32.dll",   "DestroyMenu" },
    { "user32.dll",   "GetSubMenu" },
    { "user32.dll",   "GetMenuItemCount" },
    { "user32.dll",   "GetMenuItemID" },
    { "user32.dll",   "GetSystemMenu" },
    { "user32.dll",   "CreateAcceleratorTableA" },
    { "user32.dll",   "TranslateAcceleratorA" },
    { "user32.dll",   "DestroyAcceleratorTable" },
    { "kernel32.dll", "GlobalAlloc" },
    { "kernel32.dll", "GlobalLock" },
    { "kernel32.dll", "GlobalUnlock" },
    { "kernel32.dll", "GlobalFree" },
    { "gdi32.dll",    "TextOutA" },
    { "gdi32.dll",    "FillRect" },
};

static const MyImportDescriptor g_WaitLabImports[] = {
    { "kernel32.dll", "GetCommandLineA" },
    { "kernel32.dll", "GetStartupInfoA" },
    { "kernel32.dll", "GetModuleHandleA" },
    { "kernel32.dll", "GetLastError" },
    { "kernel32.dll", "CreateEventA" },
    { "kernel32.dll", "OpenEventA" },
    { "kernel32.dll", "SetEvent" },
    { "kernel32.dll", "ResetEvent" },
    { "kernel32.dll", "CloseHandle" },
    { "kernel32.dll", "DuplicateHandle" },
    { "kernel32.dll", "WaitForSingleObject" },
    { "kernel32.dll", "WaitForMultipleObjects" },
    { "kernel32.dll", "CreateProcessA" },
    { "kernel32.dll", "TerminateProcess" },
    { "user32.dll",   "CreateWindowExA" },
    { "user32.dll",   "DestroyWindow" },
    { "gdi32.dll",    "TextOutA" },
};

static const MyImportDescriptor g_SectionLabImports[] = {
    { "kernel32.dll", "GetCommandLineA" },
    { "kernel32.dll", "GetStartupInfoA" },
    { "kernel32.dll", "GetModuleHandleA" },
    { "kernel32.dll", "GetLastError" },
    { "kernel32.dll", "CreateFileMappingA" },
    { "kernel32.dll", "OpenFileMappingA" },
    { "kernel32.dll", "MapViewOfFile" },
    { "kernel32.dll", "UnmapViewOfFile" },
    { "kernel32.dll", "FlushViewOfFile" },
    { "kernel32.dll", "CreateEventA" },
    { "kernel32.dll", "OpenEventA" },
    { "kernel32.dll", "SetEvent" },
    { "kernel32.dll", "ResetEvent" },
    { "kernel32.dll", "WaitForSingleObject" },
    { "kernel32.dll", "CloseHandle" },
    { "user32.dll",   "CreateWindowExA" },
    { "user32.dll",   "DestroyWindow" },
    { "gdi32.dll",    "TextOutA" },
};

static const MyImportDescriptor g_StateBusImports[] = {
    { "kernel32.dll", "GetCommandLineA" },
    { "kernel32.dll", "GetStartupInfoA" },
    { "kernel32.dll", "GetModuleHandleA" },
    { "kernel32.dll", "GetLastError" },
    { "kernel32.dll", "CreateFileMappingA" },
    { "kernel32.dll", "OpenFileMappingA" },
    { "kernel32.dll", "MapViewOfFile" },
    { "kernel32.dll", "UnmapViewOfFile" },
    { "kernel32.dll", "FlushViewOfFile" },
    { "kernel32.dll", "CreateEventA" },
    { "kernel32.dll", "OpenEventA" },
    { "kernel32.dll", "SetEvent" },
    { "kernel32.dll", "ResetEvent" },
    { "kernel32.dll", "WaitForSingleObject" },
    { "kernel32.dll", "CloseHandle" },
    { "user32.dll",   "CreateWindowExA" },
    { "user32.dll",   "PostMessageA" },
    { "user32.dll",   "DestroyWindow" },
    { "gdi32.dll",    "TextOutA" },
};

static const MyImportDescriptor g_ConsoleImports[] = {
    { "kernel32.dll", "GetCommandLineA" },
    { "kernel32.dll", "GetStartupInfoA" },
    { "kernel32.dll", "GetModuleHandleA" },
    { "kernel32.dll", "GetModuleFileNameA" },
    { "kernel32.dll", "GetEnvironmentVariableA" },
    { "kernel32.dll", "SetEnvironmentVariableA" },
    { "kernel32.dll", "GetLastError" },
    { "kernel32.dll", "ExitProcess" },
};

#define IMPORTS(a) (a), (DWORD)(sizeof(a)/sizeof((a)[0]))

static const MyAppEntry g_AppEntries[] = {
    { "calc",        "Rechner [OOP]",    "calculator",     CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "calc.exe",    "Rechner [OOP]",    "calculator",     CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "calculator",  "Rechner [OOP]",    "calculator",     CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "calculator.exe", "Rechner [OOP]", "calculator",     CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "editor",      "Texteditor [OOP]", "editor",         CAP_FS_READ|CAP_FS_WRITE|CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "editor.exe",  "Texteditor [OOP]", "editor",         CAP_FS_READ|CAP_FS_WRITE|CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "notepad",     "Texteditor [OOP]", "editor",         CAP_FS_READ|CAP_FS_WRITE|CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "notepad.exe", "Texteditor [OOP]", "editor",         CAP_FS_READ|CAP_FS_WRITE|CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "texteditor",  "Texteditor [OOP]", "editor",         CAP_FS_READ|CAP_FS_WRITE|CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "spy",         "myOS Spy++",       "spy",            CAP_IPC|CAP_HOOK|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_WINDOW_SUBSCRIBE|CAP_PROCESS_ENUM, app_winmain_spy, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "access-lab",  "myOS AccessLab",   "access-lab",     CAP_IPC|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_SUBSCRIBE|CAP_PROCESS_ENUM, app_winmain_access, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "pump-lab",    "myOS PumpLab",     "pump-lab",       CAP_IPC|CAP_WINDOW_READ, app_winmain_pump, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "deadlock-lab","myOS DeadlockLab", "deadlock-lab",   CAP_IPC|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_SUBSCRIBE, app_winmain_deadlock, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "section-lab", "SectionLab [OOP]", "section-lab",    CAP_IPC|CAP_EXEC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_SectionLabImports), "gui-ipc" },
    { "sectionlab",  "SectionLab [OOP]", "section-lab",    CAP_IPC|CAP_EXEC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_SectionLabImports), "gui-ipc" },
    { "section-lab-classic", "myOS SectionLab [classic]", "section-lab", CAP_IPC|CAP_SECTION_MAP|CAP_WINDOW_READ, app_winmain_section, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "statebus-lab", "StateBusLab [OOP]", "statebus-lab", CAP_IPC|CAP_EXEC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_StateBusImports), "gui-ipc" },
    { "statebus",     "StateBusLab [OOP]", "statebus-lab", CAP_IPC|CAP_EXEC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_StateBusImports), "gui-ipc" },
    { "hwndstate-lab", "HWND StateProbe [OOP v74]", "hwndstate-lab", CAP_IPC|CAP_EXEC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_WINDOW_SUBSCRIBE|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_StateBusImports), "gui-ipc" },
    { "hwndstate",     "HWND StateProbe [OOP v74]", "hwndstate-lab", CAP_IPC|CAP_EXEC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_WINDOW_SUBSCRIBE|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_StateBusImports), "gui-ipc" },
    { "surface-lab",   "SurfaceLab [OOP v75.1/v76 route]", "surface-lab", CAP_IPC|CAP_EXEC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_SectionLabImports), "gui-ipc" },
    { "surfacelab",    "SurfaceLab [OOP v75.1/v76 route]", "surface-lab", CAP_IPC|CAP_EXEC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_SectionLabImports), "gui-ipc" },
    { "object-lab",  "ObjectProbe [OOP]", "object-lab",    CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_WaitLabImports), "gui-ipc" },
    { "objectlab",   "ObjectProbe [OOP]", "object-lab",    CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_WaitLabImports), "gui-ipc" },
    { "object-lab-classic", "myOS ObjectLab [classic]", "object-lab", CAP_IPC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_PROCESS_ENUM, app_winmain_object, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "wait-lab",    "WaitLab [OOP]",     "wait-lab",       CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_WaitLabImports), "gui-ipc" },
    { "waitlab",     "WaitLab [OOP]",     "wait-lab",       CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, NULL, NULL, "WinMain/IPC", IMPORTS(g_WaitLabImports), "gui-ipc" },
    { "wait-lab-classic", "myOS WaitLab [classic]", "wait-lab", CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_PROCESS_ENUM, app_winmain_waitlab, NULL, "WinMain", IMPORTS(g_WaitLabImports), "windows" },
    { "clip-menu-lab","ClipMenuLab [OOP]", "clip-menu-lab", CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "clipmenu",    "ClipMenuLab [OOP]", "clip-menu-lab", CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "clip-menu-lab.exe","ClipMenuLab [OOP]", "clip-menu-lab", CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "paint-lab",   "PaintLab [OOP]",    "paint-lab",      CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "paintlab",    "PaintLab [OOP]",    "paint-lab",      CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "paint-lab.exe", "PaintLab [OOP]",  "paint-lab",      CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "drag-lab",    "DragLab [OOP]",    "drag-lab",       CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "draglab",     "DragLab [OOP]",    "drag-lab",       CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "drag-lab.exe", "DragLab [OOP]",    "drag-lab",       CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "control-lab", "ControlLab [OOP]",  "control-lab",    CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "controllab", "ControlLab [OOP]",  "control-lab",    CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "control-lab.exe", "ControlLab [OOP]",  "control-lab",    CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "service-lab", "myOS ServiceLab",  "service-lab",    CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM, app_winmain_servicelab, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "dialog-lab",  "DialogLab [OOP]",  "dialog-lab",     CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "dialoglab",   "DialogLab [OOP]",  "dialog-lab",     CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "dialog-lab.exe", "DialogLab [OOP]", "dialog-lab",    CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "dialog-lab-classic", "myOS DialogLab [classic]", "dialog-lab", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, app_winmain_dialoglab, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "mdi-lab",     "myOS MDILab",      "mdi-lab",        CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, app_winmain_mdilab, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "mdilab",      "myOS MDILab",      "mdi-lab",        CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, app_winmain_mdilab, NULL, "WinMain", IMPORTS(g_GuiImports), "windows" },
    { "ipc-gui-lab", "myOS IPC GUI Child", "ipc-gui-lab", CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "ipcgui",      "myOS IPC GUI Child", "ipc-gui-lab", CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL, NULL, NULL, "WinMain/IPC", IMPORTS(g_GuiImports), "gui-ipc" },
    { "argdump",     "argdump",          "argdump",        CAP_IPC|CAP_EXEC, NULL, app_main_argdump, "main", IMPORTS(g_ConsoleImports), "console" },
    { "argdump.exe", "argdump",          "argdump",        CAP_IPC|CAP_EXEC, NULL, app_main_argdump, "main", IMPORTS(g_ConsoleImports), "console" },
    { "argv-lab",    "argdump",          "argdump",        CAP_IPC|CAP_EXEC, NULL, app_main_argdump, "main", IMPORTS(g_ConsoleImports), "console" },
    { "sleeper",     "sleeper",          "sleeper",        CAP_IPC|CAP_EXEC, NULL, app_main_sleeper, "main", IMPORTS(g_ConsoleImports), "console" },
    { "sleeper.exe", "sleeper",          "sleeper",        CAP_IPC|CAP_EXEC, NULL, app_main_sleeper, "main", IMPORTS(g_ConsoleImports), "console" },
};

#undef IMPORTS

static const MyAppEntry* find_entry(LPCSTR image);

static WindowManager* g_AppHostShellWm = NULL;

BOOL MyAppHostBindShell(WindowManager* wm)
{
    g_AppHostShellWm = wm;
    return wm ? TRUE : FALSE;
}

static int ascii_ieq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static int str_endswith_i(const char* s, const char* ext)
{
    if (!s || !ext) return 0;
    size_t ns = strlen(s), ne = strlen(ext);
    if (ne > ns) return 0;
    return ascii_ieq(s + ns - ne, ext);
}

static const char* path_basename(const char* s)
{
    if (!s) return NULL;
    const char* base = s;
    for (const char* p = s; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static void shell_auto_position(WindowManager* wm, int* x, int* y)
{
    int n = wm ? wm->count : 0;
    *x = 120 + (n * 24) % 360;
    *y = 90 + (n * 22) % 260;
}

static void apphost_build_command_line(char* out, size_t cb, LPCSTR image, LPCSTR path, LPCSTR params)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    if (path && path[0] && params && params[0])
        snprintf(out, cb, "\"%s\" \"%s\" %s", image ? image : "app", path, params);
    else if (path && path[0])
        snprintf(out, cb, "\"%s\" \"%s\"", image ? image : "app", path);
    else if (params && params[0])
        snprintf(out, cb, "\"%s\" %s", image ? image : "app", params);
    else
        snprintf(out, cb, "\"%s\"", image ? image : "app");
}

#define MYAPPHOST_MAX_ARGC 16
#define MYAPPHOST_MAX_ARG_CHARS 64

static int apphost_parse_argv(LPCSTR cmd, char storage[MYAPPHOST_MAX_ARGC][MYAPPHOST_MAX_ARG_CHARS], char* argv[MYAPPHOST_MAX_ARGC])
{
    int argc = 0;
    const char* p = cmd ? cmd : "";
    while (*p && argc < MYAPPHOST_MAX_ARGC) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int quoted = 0;
        int n = 0;
        if (*p == '"') { quoted = 1; p++; }
        while (*p) {
            if (quoted) {
                if (*p == '"') { p++; break; }
            } else if (*p == ' ' || *p == '\t') break;
            if (*p == '\\' && p[1] == '"') p++;
            if (n + 1 < MYAPPHOST_MAX_ARG_CHARS) storage[argc][n++] = *p;
            p++;
        }
        storage[argc][n] = 0;
        argv[argc] = storage[argc];
        argc++;
        while (*p == ' ' || *p == '\t') p++;
    }
    return argc;
}

static void apphost_make_argv_preview(char* out, size_t cb, int argc, char* argv[MYAPPHOST_MAX_ARGC])
{
    if (!out || cb == 0) return;
    out[0] = 0;
    size_t used = 0;
    for (int i = 0; i < argc; i++) {
        int wrote = snprintf(out + used, cb - used, "%s%d='%s'", used ? ";" : "", i, argv[i] ? argv[i] : "");
        if (wrote < 0) break;
        if ((size_t)wrote >= cb - used) { out[cb - 1] = 0; break; }
        used += (size_t)wrote;
    }
}

static void apphost_child_exe_path(char* out, size_t cb)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = 0;
        char* slash = strrchr(self, '/');
        if (slash) {
            *slash = 0;
            const char* suffix = "/myos_apphost_child";
            if (strlen(self) + strlen(suffix) + 1 < cb) {
                snprintf(out, cb, "%s%s", self, suffix);
                return;
            }
        }
    }
    snprintf(out, cb, "./myos_apphost_child");
}

static BOOL apphost_fork_exec_console(const MyAppEntry* e,
                                      LPCSTR lpDirectory,
                                      int argc,
                                      char* argv[MYAPPHOST_MAX_ARGC],
                                      DWORD litePid,
                                      int* outOsPid)
{
    if (outOsPid) *outOsPid = 0;
    if (!e || litePid == 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    char exePath[512];
    apphost_child_exe_path(exePath, sizeof(exePath));

    int osPid = 0;
    if (!MyProcessHostSpawnConsole(litePid, exePath, lpDirectory, e->image, argc, argv, &osPid))
        return FALSE;

    if (!MyWinAttachLinuxProcess(litePid, osPid)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (outOsPid) *outOsPid = osPid;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}


static BOOL apphost_fork_exec_gui(const MyAppEntry* e,
                                  LPCSTR lpDirectory,
                                  LPCSTR lpTitle,
                                  LPCSTR lpPath,
                                  int x,
                                  int y,
                                  int w,
                                  int h,
                                  DWORD litePid,
                                  int* outOsPid)
{
    if (outOsPid) *outOsPid = 0;
    if (!e || litePid == 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    char exePath[512];
    apphost_child_exe_path(exePath, sizeof(exePath));

    int osPid = 0;
    if (!MyProcessHostSpawnGui(litePid, exePath, lpDirectory, e->image, lpTitle, lpPath, x, y, w, h, &osPid))
        return FALSE;

    if (!MyWinAttachLinuxProcess(litePid, osPid)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (outOsPid) *outOsPid = osPid;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static int apphost_is_console_subsystem(const MyAppEntry* e)
{
    return e && e->subsystem && ascii_ieq(e->subsystem, "console");
}

static int apphost_is_gui_ipc_subsystem(const MyAppEntry* e)
{
    return e && e->subsystem && ascii_ieq(e->subsystem, "gui-ipc");
}

static void apphost_fill_startupinfo(STARTUPINFOA* si, LPCSTR title, int x, int y, int nShow)
{
    if (!si) return;
    memset(si, 0, sizeof(*si));
    si->cb = sizeof(*si);
    si->lpTitle = (LPSTR)title;
    si->dwX = (DWORD)x;
    si->dwY = (DWORD)y;
    si->wShowWindow = (WORD)nShow;
    si->dwFlags = STARTF_USEPOSITION | STARTF_USESHOWWINDOW;
}

static void apphost_clamp_window_to_screen(WindowManager* wm, int* x, int* y, int w, int h)
{
    if (!wm || !x || !y) return;
    int sw = wm->screen_w > 0 ? wm->screen_w : 1280;
    int sh = wm->screen_h > 0 ? wm->screen_h : 800;
    int max_x = sw - (w > 80 ? 80 : w);
    int max_y = sh - TASKBAR_H - (h > 60 ? 60 : h);
    if (max_x < 0) max_x = 0;
    if (max_y < TITLEBAR_H) max_y = TITLEBAR_H;
    if (*x < 0) *x = 0;
    if (*y < TITLEBAR_H) *y = TITLEBAR_H;
    if (*x > max_x) *x = max_x;
    if (*y > max_y) *y = max_y;
}

static void apphost_apply_startup_to_window(WindowManager* wm, int index, const STARTUPINFOA* si, int nShowCmd)
{

    if (!wm || index < 0 || index >= wm->count) return;
    Window* w = &wm->wins[index];
    int show = nShowCmd;
    if (si && ((si->dwFlags & STARTF_USESHOWWINDOW) != 0)) show = si->wShowWindow;
    if (show == SW_HIDE) w->minimized = 1;
    if (si && ((si->dwFlags & STARTF_USEPOSITION) != 0)) {
        w->x = (int)si->dwX;
        w->y = (int)si->dwY;
        apphost_clamp_window_to_screen(wm, &w->x, &w->y, w->w, w->h);
    }
    // STARTF_USESIZE is persisted in Process-Lite for the future loader.
    // The current fixed-size app demos keep their HWND/client size unchanged.
}


static void apphost_preview_append(char* out, size_t cb, const char* item)
{
    if (!out || cb == 0 || !item) return;
    size_t used = strlen(out);
    if (used + 1 >= cb) return;
    int wrote = snprintf(out + used, cb - used, "%s%s", used ? ";" : "", item);
    (void)wrote;
}

static int apphost_loaded_module_index(char names[][MYWIN_MAX_MODULE_NAME], DWORD count, LPCSTR module)
{
    for (DWORD i = 0; i < count; i++)
        if (ascii_ieq(names[i], module)) return (int)i;
    return -1;
}

static BOOL apphost_resolve_imports(const MyAppEntry* e, char* preview, size_t previewCb, DWORD* resolvedOut, DWORD* errOut)
{
    if (preview && previewCb) preview[0] = 0;
    if (resolvedOut) *resolvedOut = 0;
    if (errOut) *errOut = ERROR_SUCCESS;
    if (!e) { if (errOut) *errOut = ERROR_INVALID_PARAMETER; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    HMODULE modules[8];
    char moduleNames[8][MYWIN_MAX_MODULE_NAME];
    DWORD moduleCount = 0;
    DWORD resolved = 0;

    for (DWORD i = 0; i < e->importCount; i++) {
        const MyImportDescriptor* imp = &e->imports[i];
        HMODULE h = 0;
        int midx = apphost_loaded_module_index(moduleNames, moduleCount, imp->module);
        if (midx >= 0) h = modules[midx];
        else {
            h = LoadLibraryA(imp->module);
            if (!h) {
                DWORD err = GetLastError();
                char item[96];
                snprintf(item, sizeof(item), "%.24s!%.40s=MODERR%u", imp->module, imp->proc, err);
                apphost_preview_append(preview, previewCb, item);
                if (errOut) *errOut = err ? err : ERROR_MOD_NOT_FOUND;
                return FALSE;
            }
            if (moduleCount < 8) {
                modules[moduleCount] = h;
                snprintf(moduleNames[moduleCount], sizeof(moduleNames[moduleCount]), "%s", imp->module);
                moduleCount++;
            }
        }

        FARPROC fp = GetProcAddress(h, imp->proc);
        if (!fp) {
            DWORD err = GetLastError();
            char item[96];
            snprintf(item, sizeof(item), "%.24s!%.40s=PROCERR%u", imp->module, imp->proc, err);
            apphost_preview_append(preview, previewCb, item);
            if (errOut) *errOut = err ? err : ERROR_PROC_NOT_FOUND;
            return FALSE;
        }
        resolved++;
        if (i < 5 || i + 1 == e->importCount) {
            char item[96];
            snprintf(item, sizeof(item), "%.18s!%.32s", imp->module, imp->proc);
            apphost_preview_append(preview, previewCb, item);
        }
    }

    if (resolvedOut) *resolvedOut = resolved;
    if (errOut) *errOut = ERROR_SUCCESS;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static const char* shell_resolve_image(LPCSTR lpFile, char* resolved, size_t resolved_len, LPCSTR* outPath)
{
    if (outPath) *outPath = NULL;
    if (!lpFile || !lpFile[0] || !resolved || resolved_len == 0) return NULL;

    snprintf(resolved, resolved_len, "%s", lpFile);
    for (char* p = resolved; *p; ++p) *p = (char)tolower((unsigned char)*p);

    const char* base = path_basename(resolved);
    if (find_entry(resolved)) return resolved;
    if (base && find_entry(base)) return base;

    if (str_endswith_i(lpFile, ".txt") || str_endswith_i(lpFile, ".log") ||
        str_endswith_i(lpFile, ".md")  || str_endswith_i(lpFile, ".c")   ||
        str_endswith_i(lpFile, ".h")   || str_endswith_i(lpFile, ".ini") ||
        str_endswith_i(lpFile, ".cfg")) {
        if (outPath) *outPath = lpFile;
        snprintf(resolved, resolved_len, "editor");
        return resolved;
    }

    return NULL;
}

static BOOL shell_execute_resolved(WindowManager* wm, LPCSTR lpFile, LPCSTR lpVerb, LPCSTR lpParameters, LPCSTR lpDirectory, int nShow, MyAppLaunchResult* result, HINSTANCE* hInstApp)
{
    if (hInstApp) *hInstApp = (HINSTANCE)SE_ERR_FNF;
    if (!wm || !lpFile || !lpFile[0]) { SetLastError(!wm ? ERROR_INVALID_HANDLE : ERROR_FILE_NOT_FOUND); return FALSE; }
    if (lpVerb && lpVerb[0] && !ascii_ieq(lpVerb, "open")) {
        if (hInstApp) *hInstApp = (HINSTANCE)SE_ERR_ACCESSDENIED;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    char image[96];
    LPCSTR path = NULL;
    const char* resolved = shell_resolve_image(lpFile, image, sizeof(image), &path);
    if (!resolved) {
        if (hInstApp) *hInstApp = (HINSTANCE)SE_ERR_NOASSOC;
        SetLastError(ERROR_NO_ASSOCIATION);
        return FALSE;
    }

    int x, y;
    shell_auto_position(wm, &x, &y);
    char title[96];
    if (path && path[0]) snprintf(title, sizeof(title), "Texteditor - %.70s", path_basename(path));
    else snprintf(title, sizeof(title), "%s", resolved);

    STARTUPINFOA si;
    apphost_fill_startupinfo(&si, title, x, y, nShow);

    MyAppLaunchOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.lpImageName = resolved;
    opt.lpTitle = title;
    opt.lpPath = path;
    opt.lpParameters = lpParameters;
    opt.lpDirectory = lpDirectory;
    opt.x = x;
    opt.y = y;
    opt.nShowCmd = nShow;
    opt.lpStartupInfo = &si;

    if (!MyAppHostLaunchEx(wm, &opt, result)) {
        if (hInstApp) *hInstApp = (HINSTANCE)SE_ERR_ACCESSDENIED;
        if (GetLastError() == ERROR_SUCCESS) SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (hInstApp) *hInstApp = MYOS_SHELLEXECUTE_SUCCESS;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

HINSTANCE ShellExecuteA(HWND hwnd, LPCSTR lpOperation, LPCSTR lpFile, LPCSTR lpParameters, LPCSTR lpDirectory, int nShowCmd)
{
    (void)hwnd;
    HINSTANCE h = 0;
    MyAppLaunchResult r;
    if (!shell_execute_resolved(g_AppHostShellWm, lpFile, lpOperation, lpParameters, lpDirectory, nShowCmd, &r, &h))
        return h;

    // AppHost keeps hProcess/hThread owned by the desktop frame.
    // ShellExecuteA only reports success/failure, matching the public WinAPI contract.
    if (r.window_index < 0) {
        if (r.hThread) CloseHandle(r.hThread);
        if (r.hProcess) CloseHandle(r.hProcess);
    }
    return h;
}

BOOL ShellExecuteExA(LPSHELLEXECUTEINFOA lpExecInfo)
{
    if (!lpExecInfo || lpExecInfo->cbSize < sizeof(SHELLEXECUTEINFOA)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    lpExecInfo->hInstApp = (HINSTANCE)SE_ERR_FNF;
    lpExecInfo->hProcess = 0;

    MyAppLaunchResult r;
    memset(&r, 0, sizeof(r));
    BOOL ok = shell_execute_resolved(g_AppHostShellWm, lpExecInfo->lpFile, lpExecInfo->lpVerb, lpExecInfo->lpParameters, lpExecInfo->lpDirectory, lpExecInfo->nShow, &r, &lpExecInfo->hInstApp);
    if (!ok) return FALSE;

    if (lpExecInfo->fMask & SEE_MASK_NOCLOSEPROCESS) {
        if (r.window_index < 0 && r.process_id) {
            lpExecInfo->hProcess = OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.process_id);
        } else {
            HANDLE dup = 0;
            if (r.hProcess && DuplicateHandle(GetCurrentProcess(), r.hProcess, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
                lpExecInfo->hProcess = dup;
            else
                lpExecInfo->hProcess = r.hProcess;
        }
    }
    // GUI AppHost/window keeps original process/thread handles for close/terminate.
    // Console images have no desktop frame, so release the loader-owned originals
    // after optionally duplicating the public waitable hProcess for the caller.
    if (r.window_index < 0) {
        if (r.hThread) CloseHandle(r.hThread);
        if (r.hProcess && lpExecInfo->hProcess != r.hProcess) CloseHandle(r.hProcess);
    }
    return TRUE;
}

static const MyAppEntry* find_entry(LPCSTR image)
{
    if (!image || !image[0]) return NULL;
    for (unsigned i = 0; i < sizeof(g_AppEntries)/sizeof(g_AppEntries[0]); i++)
        if (strcmp(g_AppEntries[i].image, image) == 0) return &g_AppEntries[i];
    return NULL;
}

BOOL MyAppHostIsRegistered(LPCSTR lpImageName)
{
    return find_entry(lpImageName) ? TRUE : FALSE;
}

static void apphost_fail_process_info(DWORD ownerPid, PROCESS_INFORMATION* pi, DWORD exitCode)
{
    if (!pi) return;
    if (ownerPid && MyWinEnterProcessContext(ownerPid)) {
        if (pi->hProcess) TerminateProcess(pi->hProcess, exitCode);
        if (pi->hThread) CloseHandle(pi->hThread);
        if (pi->hProcess) CloseHandle(pi->hProcess);
        MyWinLeaveProcessContext();
    }
    pi->hThread = 0;
    pi->hProcess = 0;
}

static void apphost_attach_window_process(WindowManager* wm, int index, const MyAppEntry* e, const PROCESS_INFORMATION* pi, DWORD ownerPid)
{
    if (!wm || !pi || index < 0 || index >= wm->count) return;
    Window* w = &wm->wins[index];
    w->process_id = pi->dwProcessId;
    w->thread_id = pi->dwThreadId;
    w->process_handle = pi->hProcess;
    w->thread_handle = pi->hThread;
    w->process_handle_owner_pid = ownerPid;
    snprintf(w->image_name, sizeof(w->image_name), "%s", e && e->image ? e->image : "apphost-child");
}

BOOL MyAppHostLaunchEx(WindowManager* wm,
                       const MyAppLaunchOptions* lpOptions,
                       MyAppLaunchResult* lpResult)
{
    if (lpResult) { memset(lpResult, 0, sizeof(*lpResult)); lpResult->window_index = -1; }
    if (!wm || !wm->mgr || !lpOptions || !lpOptions->lpImageName) return FALSE;

    const MyAppEntry* e = find_entry(lpOptions->lpImageName);
    if (!e) return FALSE;

    int x = lpOptions->x;
    int y = lpOptions->y;
    if (x == CW_USEDEFAULT || y == CW_USEDEFAULT) shell_auto_position(wm, &x, &y);
    if (lpOptions->lpStartupInfo && ((lpOptions->lpStartupInfo->dwFlags & STARTF_USEPOSITION) != 0)) {
        x = (int)lpOptions->lpStartupInfo->dwX;
        y = (int)lpOptions->lpStartupInfo->dwY;
    }

    char title[96];
    if (lpOptions->lpTitle && lpOptions->lpTitle[0]) snprintf(title, sizeof(title), "%s", lpOptions->lpTitle);
    else if (lpOptions->lpStartupInfo && lpOptions->lpStartupInfo->lpTitle && lpOptions->lpStartupInfo->lpTitle[0]) snprintf(title, sizeof(title), "%s", lpOptions->lpStartupInfo->lpTitle);
    else snprintf(title, sizeof(title), "%s", e->defaultTitle);

    char commandLine[256];
    apphost_build_command_line(commandLine, sizeof(commandLine), e->image, lpOptions->lpPath, lpOptions->lpParameters);

    STARTUPINFOA localSi;
    LPSTARTUPINFOA si = lpOptions->lpStartupInfo;
    if (!si) {
        apphost_fill_startupinfo(&localSi, title, x, y, lpOptions->nShowCmd);
        si = &localSi;
    }

    Capability child = cap_create(0, e->capName, e->capFlags);
    cap_add_target(&child, 0);
    if (lpOptions->lpPath && lpOptions->lpPath[0]) cap_add_path(&child, lpOptions->lpPath);
    if (lpOptions->lpDirectory && lpOptions->lpDirectory[0]) cap_add_path(&child, lpOptions->lpDirectory);
    if (wm->desktop_path[0]) cap_add_path(&child, wm->desktop_path);

    Capability previous;
    int hadPrevious = 0;
    const Capability* cur = MyWinGetCurrentCapability();
    if (cur) { previous = *cur; hadPrevious = 1; }

    /* v147: loader privilege is a capability overlay, not a separate process
       identity.  Keeping the caller PID is required now that public KERNEL32
       handles are strict table handles; otherwise AppHost would create
       hProcess/hThread in pid 55 and return them to the caller's pid. */
    DWORD loaderPid = (cur && cur->id) ? cur->id : 55u;
    Capability loader = cap_create(loaderPid, "apphost-loader", CAP_EXEC|CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM|CAP_SECTION_MAP);
    cap_add_target(&loader, 0);
    MyWinBindRuntime(wm->mgr, &loader);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    if (!MyWinCreateProcessWithStartupCapability(e->image, commandLine, &child, FALSE, lpOptions->lpDirectory, si, &pi)) {
        if (hadPrevious) MyWinBindRuntime(wm->mgr, &previous);
        return FALSE;
    }

    int index = -1;
    DWORD resolvedImports = 0;
    DWORD loaderError = ERROR_INVALID_HANDLE;
    BOOL entryCalled = FALSE;
    char importPreview[MYWIN_MAX_LOADER_PREVIEW];
    importPreview[0] = 0;

    if (MyWinEnterProcessContext(pi.dwProcessId)) {
        Capability runCap = child;
        const Capability* runtimeCap = MyWinGetCurrentCapability();
        if (runtimeCap) runCap = *runtimeCap;

        memset(&g_AppEntryFrame, 0, sizeof(g_AppEntryFrame));
        g_AppEntryFrame.wm = wm;
        g_AppEntryFrame.x = x;
        g_AppEntryFrame.y = y;
        snprintf(g_AppEntryFrame.title, sizeof(g_AppEntryFrame.title), "%s", title);
        g_AppEntryFrame.path = lpOptions->lpPath;
        g_AppEntryFrame.cap = runCap;

        if (apphost_resolve_imports(e, importPreview, sizeof(importPreview), &resolvedImports, &loaderError)) {
            LPSTR cmdLine = GetCommandLineA();
            int show = si ? (int)si->wShowWindow : lpOptions->nShowCmd;
            if (apphost_is_gui_ipc_subsystem(e)) {
                entryCalled = TRUE;
                MyWinSetProcessSubsystemInfo(pi.dwProcessId, e->subsystem, 0, "", STILL_ACTIVE);
                int osPid = 0;
                int reqW = (si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize) ? (int)si->dwXSize : 360;
                int reqH = (si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize) ? (int)si->dwYSize : 180;
                if (e->image && (ascii_ieq(e->image, "calculator") || ascii_ieq(e->image, "calc"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 320;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 420;
                } else if (e->image && ascii_ieq(e->image, "editor")) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 540;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 380;
                } else if (e->image && (ascii_ieq(e->image, "paint-lab") || ascii_ieq(e->image, "paintlab") || ascii_ieq(e->image, "paint-lab.exe"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 560;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 380;
                } else if (e->image && (ascii_ieq(e->image, "drag-lab") || ascii_ieq(e->image, "draglab") || ascii_ieq(e->image, "drag-lab.exe"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 640;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 390;
                } else if (e->image && (ascii_ieq(e->image, "dialog-lab") || ascii_ieq(e->image, "dialoglab") || ascii_ieq(e->image, "dialog-lab.exe"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 920;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 320;
                } else if (e->image && (ascii_ieq(e->image, "control-lab") || ascii_ieq(e->image, "controllab") || ascii_ieq(e->image, "control-lab.exe"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 520;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 260;
                } else if (e->image && (ascii_ieq(e->image, "clip-menu-lab") || ascii_ieq(e->image, "clipmenu") || ascii_ieq(e->image, "clip-menu-lab.exe"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 640;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 360;
                } else if (e->image && (ascii_ieq(e->image, "wait-lab") || ascii_ieq(e->image, "waitlab") || ascii_ieq(e->image, "wait-lab.exe"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 760;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 310;
                } else if (e->image && (ascii_ieq(e->image, "statebus-lab") || ascii_ieq(e->image, "statebus") || ascii_ieq(e->image, "statebus-lab.exe"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 860;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 380;
                } else if (e->image && (ascii_ieq(e->image, "hwndstate-lab") || ascii_ieq(e->image, "hwndstate") || ascii_ieq(e->image, "hwndstate-lab.exe"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 900;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 430;
                } else if (e->image && (ascii_ieq(e->image, "object-lab") || ascii_ieq(e->image, "objectlab") || ascii_ieq(e->image, "object-lab.exe"))) {
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwXSize)) reqW = 700;
                    if (!(si && (si->dwFlags & STARTF_USESIZE) && si->dwYSize)) reqH = 270;
                }
                if (apphost_fork_exec_gui(e, lpOptions->lpDirectory, title, lpOptions->lpPath, x, y, reqW, reqH, pi.dwProcessId, &osPid)) {
                    MyProcessHostCreateWindowRequest req;
                    int gotReq = 0;
                    for (int spin = 0; spin < 80; spin++) {
                        BOOL exited = FALSE;
                        MyProcessHostPoll(pi.dwProcessId, &exited, NULL, NULL);
                        if (MyProcessHostTakeCreateWindowRequest(pi.dwProcessId, &req)) { gotReq = 1; break; }
                        if (exited) break;
                        usleep(10000);
                    }
                    if (gotReq) {
                        index = wm_add_ipc_proxy(wm, req.x, req.y, req.w, req.h, req.title, req.class_name, runCap, req.linux_pid, "parent executed CreateWindowExA from IPC", req.owner_hwnd, req.style, req.ex_style);
                        HWND createdHwnd = (index >= 0 && index < wm->count) ? wm->wins[index].app_hwnd : 0;
                        MyProcessHostAckCreateWindow(pi.dwProcessId, createdHwnd, (DWORD)(index >= 0 ? index : 0), index >= 0, index >= 0 ? "parent CreateWindowExA OK" : "parent CreateWindowExA failed");
                        loaderError = (index >= 0) ? ERROR_SUCCESS : ERROR_INVALID_FUNCTION;
                        char extra[96];
                        snprintf(extra, sizeof(extra), ";gui-fork=%d;hwnd=%u", osPid, (unsigned)createdHwnd);
                        apphost_preview_append(importPreview, sizeof(importPreview), extra);
                    } else {
                        loaderError = ERROR_INVALID_FUNCTION;
                        apphost_preview_append(importPreview, sizeof(importPreview), ";gui-request-timeout");
                    }
                } else {
                    loaderError = GetLastError() ? GetLastError() : ERROR_INVALID_FUNCTION;
                }
            } else if (apphost_is_console_subsystem(e)) {
                char argStorage[MYAPPHOST_MAX_ARGC][MYAPPHOST_MAX_ARG_CHARS];
                char* argv[MYAPPHOST_MAX_ARGC];
                char argvPreview[MYWIN_MAX_ARGV_PREVIEW];
                int argc = apphost_parse_argv(cmdLine, argStorage, argv);
                apphost_make_argv_preview(argvPreview, sizeof(argvPreview), argc, argv);
                entryCalled = TRUE;
                MyWinSetProcessSubsystemInfo(pi.dwProcessId, e->subsystem, (DWORD)argc, argvPreview, STILL_ACTIVE);
                int osPid = 0;
                if (apphost_fork_exec_console(e, lpOptions->lpDirectory, argc, argv, pi.dwProcessId, &osPid)) {
                    loaderError = ERROR_SUCCESS;
                    char extra[64];
                    snprintf(extra, sizeof(extra), ";fork=%d", osPid);
                    apphost_preview_append(importPreview, sizeof(importPreview), extra);
                } else {
                    loaderError = GetLastError() ? GetLastError() : ERROR_INVALID_FUNCTION;
                }
                index = -1;
            } else {
                HINSTANCE hInst = GetModuleHandleA(NULL);
                entryCalled = TRUE;
                index = e->winEntry ? e->winEntry(hInst, 0, cmdLine, show) : -1;
                loaderError = (index >= 0) ? ERROR_SUCCESS : ERROR_INVALID_FUNCTION;
                MyWinSetProcessSubsystemInfo(pi.dwProcessId, e->subsystem, 0, "", STILL_ACTIVE);
            }
        }

        MyWinSetProcessLoaderInfo(pi.dwProcessId, e->importCount, resolvedImports, loaderError, e->entryName, importPreview, entryCalled);
        memset(&g_AppEntryFrame, 0, sizeof(g_AppEntryFrame));
        MyWinLeaveProcessContext();
    } else {
        MyWinSetProcessLoaderInfo(pi.dwProcessId, e->importCount, 0, loaderError, e->entryName, "enter-context failed", FALSE);
    }

    if (hadPrevious) MyWinBindRuntime(wm->mgr, &previous);

    if (apphost_is_console_subsystem(e)) {
        // v58: console subsystem is backed by real fork/exec plus ProcessHost IPC.
        // Do NOT TerminateProcess here; waitpid bridge in MyWinPollProcess() makes
        // the PROCESS/THREAD objects signaled when the Linux child exits.
        if (loaderError != ERROR_SUCCESS) {
            apphost_fail_process_info(loaderPid, &pi, loaderError);
        }
        if (lpResult) {
            lpResult->window_index = -1;
            lpResult->process_id = pi.dwProcessId;
            lpResult->thread_id = pi.dwThreadId;
            lpResult->hProcess = pi.hProcess;
            lpResult->hThread = pi.hThread;
            snprintf(lpResult->image_name, sizeof(lpResult->image_name), "%s", e->image);
            snprintf(lpResult->title, sizeof(lpResult->title), "%.63s", title);
        }
        if (loaderError != ERROR_SUCCESS) SetLastError(loaderError ? loaderError : ERROR_INVALID_FUNCTION);
        return loaderError == ERROR_SUCCESS ? TRUE : FALSE;
    }

    if (index < 0) {
        DWORD failErr = loaderError ? loaderError : ERROR_INVALID_FUNCTION;
        apphost_fail_process_info(loaderPid, &pi, failErr);
        SetLastError(failErr);
        return FALSE;
    }

    apphost_attach_window_process(wm, index, e, &pi, loaderPid);
    apphost_apply_startup_to_window(wm, index, si, lpOptions->nShowCmd);

    if (lpResult) {
        lpResult->window_index = index;
        lpResult->process_id = pi.dwProcessId;
        lpResult->thread_id = pi.dwThreadId;
        lpResult->hProcess = pi.hProcess;
        lpResult->hThread = pi.hThread;
        snprintf(lpResult->image_name, sizeof(lpResult->image_name), "%s", e->image);
        snprintf(lpResult->title, sizeof(lpResult->title), "%s", wm->wins[index].title);
    }
    return TRUE;
}

BOOL MyAppHostLaunch(WindowManager* wm,
                     LPCSTR lpImageName,
                     int x,
                     int y,
                     LPCSTR lpTitle,
                     LPCSTR lpPath,
                     MyAppLaunchResult* lpResult)
{
    MyAppLaunchOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.lpImageName = lpImageName;
    opt.lpTitle = lpTitle;
    opt.lpPath = lpPath;
    opt.x = x;
    opt.y = y;
    opt.nShowCmd = SW_SHOW;
    return MyAppHostLaunchEx(wm, &opt, lpResult);
}
