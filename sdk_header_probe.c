/*
 * v118 SDK compile/link probe.
 *
 * This file intentionally compiles as part of the normal build. It proves that
 * the MSDN-style header names provide real public declarations, not just a
 * mywin.h include bridge.
 */
#include <windows.h>
#include <commdlg.h>
#include <winsvc.h>
#include <commctrl.h>
#include <shellapi.h>

static int myos_sdk_header_probe(void)
{
    WNDCLASSEXA wc;
    MSG msg;
    OPENFILENAMEA ofn;
    CHOOSEFONTA cf;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SC_HANDLE scm;
    RECT rc;
    SECURITY_ATTRIBUTES sa;
    PAINTSTRUCT ps;
    LOGFONTA lf;
    MENUITEMINFOA mii;
    SCROLLINFO sbi;
    SHELLEXECUTEINFOA sei;

    BOOL (*pCloseHandle)(HANDLE) = CloseHandle;
    DWORD (*pWaitForSingleObject)(HANDLE, DWORD) = WaitForSingleObject;
    HWND (*pCreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) = CreateWindowExA;
    BOOL (*pGetOpenFileNameA)(LPOPENFILENAMEA) = GetOpenFileNameA;
    BOOL (*pChooseFontA)(LPCHOOSEFONTA) = ChooseFontA;
    UINT_PTR (*pSetTimer)(HWND, UINT_PTR, UINT, TIMERPROC) = SetTimer;
    BOOL (*pKillTimer)(HWND, UINT_PTR) = KillTimer;
    SC_HANDLE (*pOpenSCManagerA)(LPCSTR, LPCSTR, DWORD) = OpenSCManagerA;
    HINSTANCE (*pShellExecuteA)(HWND,LPCSTR,LPCSTR,LPCSTR,LPCSTR,int) = ShellExecuteA;

    (void)wc; (void)msg; (void)ofn; (void)cf; (void)si; (void)pi; (void)scm;
    (void)rc; (void)sa; (void)ps; (void)lf; (void)mii; (void)sbi; (void)sei;
    (void)pCloseHandle; (void)pWaitForSingleObject; (void)pCreateWindowExA;
    (void)pGetOpenFileNameA; (void)pChooseFontA; (void)pSetTimer; (void)pKillTimer;
    (void)pOpenSCManagerA; (void)pShellExecuteA;

    /* constants should be visible through canonical SDK headers */
    return (int)(ERROR_INVALID_HANDLE + WAIT_TIMEOUT + WS_VISIBLE + CF_SCREENFONTS +
                 OFN_EXPLORER + SERVICE_RUNNING + BM_CLICK + LB_ADDSTRING + LB_DIR + LBN_SELCHANGE + CB_ADDSTRING + CB_DIR + CBN_SELCHANGE +
                 SBS_VERT + SBM_SETPOS + SC_CLOSE + SEE_MASK_NOCLOSEPROCESS + SE_ERR_NOASSOC);
}

int myos_sdk_header_probe_public_symbol(void)
{
    return myos_sdk_header_probe();
}
