#pragma once

#ifndef MYOS_CACHELINE_BYTES
#define MYOS_CACHELINE_BYTES 64u
#endif
#ifndef MYOS_CACHELINE_SIZE
#define MYOS_CACHELINE_SIZE MYOS_CACHELINE_BYTES
#endif
#ifndef MYOS_LIKELY
#if defined(__GNUC__) || defined(__clang__)
#define MYOS_LIKELY(x)   __builtin_expect(!!(x), 1)
#define MYOS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define MYOS_LIKELY(x)   (x)
#define MYOS_UNLIKELY(x) (x)
#endif
#endif
#ifndef MYOS_CACHELINE_ALIGN
#if defined(__GNUC__) || defined(__clang__)
#define MYOS_CACHELINE_ALIGN __attribute__((aligned(MYOS_CACHELINE_BYTES)))
#else
#define MYOS_CACHELINE_ALIGN
#endif
#endif
#ifndef MYOS_IS_POW2_U32
#define MYOS_IS_POW2_U32(x) ((x) && (((x) & ((x) - 1u)) == 0u))
#endif

/*
 * myOS private runtime surface.
 *
 * Include this only from the OS/runtime/lab code. Normal Win32-style app
 * sources should prefer <windows.h> and stay on the MSDN contract.
 */
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <winsvc.h>
#include <shellapi.h>
#include "capability.h"
#include "fb.h"
#include "hwnd.h"
#include "window.h"
#include "mywindow_state.h"
#include "myobject.h"
#include "myos_diag.h"


#ifdef __cplusplus
extern "C" {
#endif

#define MYOS_DLG_CLASS_BUTTON    0x0080u
#define MYOS_DLG_CLASS_EDIT      0x0081u
#define MYOS_DLG_CLASS_STATIC    0x0082u
#define MYOS_DLG_CLASS_LISTBOX   0x0083u
#define MYOS_DLG_CLASS_SCROLLBAR 0x0084u
#define MYOS_DLG_CLASS_COMBOBOX  0x0085u

#define MYOS_KEYSTATE_SHIFT 0x0001u
#define MYOS_KEYSTATE_CTRL  0x0100u
#define MYOS_KEYSTATE_ALT   0x0200u

BOOL MyWinBindRuntime(HWNDManager* lpHwndManager, const Capability* lpCapability);
BOOL MyWinBindDesktop(WindowManager* lpWindowManager);
void MyWinUnbindRuntime(void);
const Capability* MyWinGetCurrentCapability(void);
BOOL MyWinEnsureSessionInputRuntime(HWNDManager* lpHwndManager, WindowManager* lpWindowManager, const Capability* lpPreferredCapability);
HWNDManager* MyWinGetHwndManager(void);
void MyUser32CleanupProcessClasses(DWORD dwProcessId);
BOOL MyWinEnterProcessContext(DWORD dwProcessId);
BOOL MyWinLeaveProcessContext(void);
DWORD MyWinGetRuntimeContextDepth(void);

/* Internal compositor integration hook: modal USER32 loops must keep the
   myOS framebuffer compositor alive while the caller is blocked inside
   DialogBoxIndirectParamA / GetOpenFileNameA. */
typedef void (*MYWIN_MODALIDLEPROC)(void* lpContext);
void MyWinSetModalIdleProc(MYWIN_MODALIDLEPROC lpProc, void* lpContext);

void MyWinSetKeyDown(int vKey, BOOL bDown);
BOOL MyIsDialogWindow(HWND hWnd);
BOOL MyIsModelessDialog(HWND hDlg);
DWORD MyWinGetModelessDialogCount(void);
BOOL MyTranslateModelessDialogMessageA(LPMSG lpMsg);
BOOL MyGetModelessDialogAudit(DWORD* registered, DWORD* unregistered, DWORD* live, DWORD* pumpHits, DWORD* pumpMisses);
BOOL MyWinQueryMessageFilterStage(DWORD dwStage, DWORD* lpOrder, DWORD* lpCanHandle, DWORD* lpRequiredAction);
BOOL MyIsOwnedDialogChild(HWND hOwner, HWND hWnd);
HWND MyGetDialogOwner(HWND hWnd);
BOOL MyTopLevelDialogHitTest(HWND hOwner, int screenX, int screenY, HWND* lpHitHwnd);
BOOL RegisterDialogTemplateA(LPCSTR lpTemplateName, LPCDLGTEMPLATEA lpTemplate);
LPCDLGTEMPLATEA FindDialogTemplateA(LPCSTR lpTemplateName);
INT_PTR MyDialogBoxParamA(LPCSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);

BOOL MyWinRegisterResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType, LPCVOID lpData, DWORD cbData);
BOOL MyWinRegisterDialogTemplateResourceA(HMODULE hModule, LPCSTR lpName, LPCDLGTEMPLATEA lpTemplate, DWORD cbTemplate);
BOOL MyDrawChildWindows(HWND hWndParent, Framebuffer* fb, int xOrigin, int yOrigin);
BOOL MyDrawStandardScrollBars(HWND hWnd, Framebuffer* fb, int clientX, int clientY, int clientW, int clientH);
BOOL MyDrawTopLevelDialogs(Framebuffer* fb);
BOOL MySubscribeWindowMessage(HWND hWndSource, HWND hWndSubscriber, UINT wMsgFilterMin, UINT wMsgFilterMax);
BOOL MyUnsubscribeWindowMessage(HWND hWndSource, HWND hWndSubscriber, UINT wMsgFilterMin, UINT wMsgFilterMax);

BOOL   MyWinGetSectionBackingInfo(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, DWORD dwNumberOfBytesToMap, LPSTR lpShmName, DWORD cchShmName, DWORD* lpMapBytes, DWORD* lpSectionSize, DWORD* lpProtect);
BOOL   MyWinReleaseSectionViewHandle(HANDLE hFileMappingObject);
BOOL   MyWinCreateProcessWithStartupCapability(LPCSTR lpApplicationName, LPSTR lpCommandLine, const Capability* lpChildCapability, BOOL bInheritHandles, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);

/* GDI diagnostics / compositor integration hooks. */
typedef struct tagMYGDI_WINDOW_SNAPSHOT {
    BOOL  dirty;
    BOOL  paintPending;
    BOOL  erasePending;
    BOOL  internalPaint;
    RECT  dirtyRect;
    DWORD invalidateSerial;
    DWORD postedPaints;
    DWORD coalescedInvalidates;
} MYGDI_WINDOW_SNAPSHOT;

BOOL    MyGdiGetWindowState(HWND hWnd, MYGDI_WINDOW_SNAPSHOT* lpOut);
DWORD   MyGdiGetBrushSelectedCount(HBRUSH hBrush);
void    MyGdiBlitWindow(HWND hWnd, int clientX, int clientY, int clientW, int clientH, Framebuffer* fb);
BOOL    MyGdiScrollWindowContent(HWND hWnd, int dx, int dy, const RECT* prcScroll, const RECT* prcClip);
void    MyGdiReleaseWindow(HWND hWnd);


/* Private service-lab aliases and diagnostics. Public SCM entry points live in <winsvc.h>. */
#define MYSVC_MAX_SERVICES 64
#define MYSVC_STOPPED          SERVICE_STOPPED
#define MYSVC_START_PENDING    SERVICE_START_PENDING
#define MYSVC_STOP_PENDING     SERVICE_STOP_PENDING
#define MYSVC_RUNNING          SERVICE_RUNNING
#define MYSVC_PAUSED           SERVICE_PAUSED
#define MYSVC_START_TYPE_DEMAND SERVICE_DEMAND_START
#define MYSVC_START_TYPE_AUTO   SERVICE_AUTO_START
#define MYSVC_ACCEPT_STOP       SERVICE_ACCEPT_STOP
#define MYSVC_ACCEPT_PAUSE      SERVICE_ACCEPT_PAUSE_CONTINUE
#define MYSVC_FLAG_RUNNING      0x00000001u
#define MYSVC_FLAG_AUTO_START   0x00000002u
#define MYSVC_FLAG_MARKED_DELETE 0x00000004u
#define MYSVC_ACCESS_QUERY      0x0001u
#define MYSVC_ACCESS_START      SERVICE_START
#define MYSVC_ACCESS_STOP       SERVICE_STOP
#define MYSVC_ACCESS_CHANGE     0x0040u
#define MYSVC_ACCESS_ALL        SERVICE_ALL_ACCESS

typedef BOOL (*MYSVCENUMPROC)(const MyServiceInfo* lpInfo, LPARAM lParam);
void      MySvcInit(void);
BOOL      MySvcEnumServices(MYSVCENUMPROC lpEnumFunc, LPARAM lParam);
DWORD     MySvcGetCount(void);
DWORD     MySvcGetRunningCount(void);
const char* MySvcStateName(DWORD state);

#ifdef __cplusplus
}
#endif
