#pragma once
/*
 * myOS Win32 SDK - winuser.h
 * Public USER32 declarations. v130 keeps MSG on the Win32/MSDN public
 * contract: no private queue payload is embedded in the SDK-visible layout.
 */
#include "windef.h"
#include "winnt.h"
#include "wingdi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_USEDEFAULT ((int)0x80000000u)

#define SMTO_NORMAL             0x0000u
#define SMTO_BLOCK              0x0001u
#define SMTO_ABORTIFHUNG        0x0002u
#define SMTO_NOTIMEOUTIFNOTHUNG 0x0008u

#define PM_NOREMOVE 0x0000u
#define PM_REMOVE   0x0001u

#define QS_KEY          0x0001u
#define QS_MOUSEMOVE    0x0002u
#define QS_MOUSEBUTTON  0x0004u
#define QS_POSTMESSAGE  0x0008u
#define QS_TIMER        0x0010u
#define QS_PAINT        0x0020u
#define QS_SENDMESSAGE  0x0040u
#define QS_HOTKEY       0x0080u
#define QS_ALLPOSTMESSAGE 0x0100u
#define QS_RAWINPUT     0x0400u
#define QS_MOUSE        (QS_MOUSEMOVE|QS_MOUSEBUTTON)
#define QS_INPUT        (QS_MOUSE|QS_KEY|QS_RAWINPUT)
#define QS_ALLEVENTS    (QS_INPUT|QS_POSTMESSAGE|QS_TIMER|QS_PAINT|QS_HOTKEY)
#define QS_ALLINPUT     (QS_INPUT|QS_POSTMESSAGE|QS_TIMER|QS_PAINT|QS_HOTKEY|QS_SENDMESSAGE)

#define MWMO_WAITALL        0x0001u
#define MWMO_ALERTABLE      0x0002u
#define MWMO_INPUTAVAILABLE 0x0004u


#define SW_HIDE     0
#define SW_SHOWNORMAL 1
#define SW_NORMAL   1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW     5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA   8
#define SW_RESTORE  9
#define SW_SHOWDEFAULT 10

#define WS_OVERLAPPED 0x00000000u
#define WS_VISIBLE    0x10000000u
#define WS_CHILD      0x40000000u
#define WS_DISABLED   0x08000000u
#define WS_TABSTOP    0x00010000u
#define WS_GROUP      0x00020000u
#define WS_HSCROLL    0x00100000u
#define WS_VSCROLL    0x00200000u
#define WS_BORDER     0x00800000u
#define WS_CAPTION    0x00C00000u
#define WS_SYSMENU    0x00080000u
#define WS_OVERLAPPEDWINDOW 0x00CF0000u
#define WS_EX_NONE        0x00000000u
#define WS_EX_TRANSPARENT 0x00000020u

#define SWP_NOSIZE       0x0001u
#define SWP_NOMOVE       0x0002u
#define SWP_NOZORDER     0x0004u
#define SWP_NOREDRAW     0x0008u
#define SWP_NOACTIVATE   0x0010u
#define SWP_FRAMECHANGED 0x0020u
#define SWP_SHOWWINDOW   0x0040u
#define SWP_HIDEWINDOW   0x0080u
#define SWP_NOCOPYBITS   0x0100u
#define HWND_TOP         ((HWND)0)
#define HWND_BOTTOM      ((HWND)1)
#define HWND_TOPMOST     ((HWND)0xFFFFFFFEu)
#define HWND_NOTOPMOST   ((HWND)0xFFFFFFFDu)

#define RDW_INVALIDATE      0x0001u
#define RDW_INTERNALPAINT   0x0002u
#define RDW_ERASE           0x0004u
#define RDW_VALIDATE        0x0008u
#define RDW_NOINTERNALPAINT 0x0010u
#define RDW_NOERASE         0x0020u
#define RDW_NOCHILDREN      0x0040u
#define RDW_ALLCHILDREN     0x0080u
#define RDW_UPDATENOW       0x0100u
#define RDW_ERASENOW        0x0200u
#define RDW_FRAME           0x0400u
#define RDW_NOFRAME         0x0800u

#define CWP_ALL             0x0000u
#define CWP_SKIPINVISIBLE   0x0001u
#define CWP_SKIPDISABLED    0x0002u
#define CWP_SKIPTRANSPARENT 0x0004u

#ifndef ERROR
#define ERROR 0
#endif
#ifndef NULLREGION
#define NULLREGION 1
#endif
#ifndef SIMPLEREGION
#define SIMPLEREGION 2
#endif
#ifndef COMPLEXREGION
#define COMPLEXREGION 3
#endif

#define SW_SCROLLCHILDREN 0x0001u
#define SW_INVALIDATE     0x0002u
#define SW_ERASE          0x0004u
#define SW_SMOOTHSCROLL   0x0010u

#define GW_HWNDFIRST 0u
#define GW_HWNDLAST  1u
#define GW_HWNDNEXT  2u
#define GW_HWNDPREV  3u
#define GW_OWNER     4u
#define GW_CHILD     5u
#define GA_PARENT 1u
#define GA_ROOT   2u
#define GA_ROOTOWNER 3u

#define GWL_STYLE       (-16)
#define GWL_EXSTYLE     (-20)
#define GWLP_WNDPROC    (-4)
#define GWLP_HINSTANCE  (-6)
#define GWLP_HWNDPARENT (-8)
#define GWLP_ID         (-12)
#define GWLP_USERDATA   (-21)

#define GCL_MENUNAME       (-8)
#define GCL_HBRBACKGROUND  (-10)
#define GCL_HCURSOR        (-12)
#define GCL_HICON          (-14)
#define GCL_HMODULE        (-16)
#define GCL_CBWNDEXTRA     (-18)
#define GCL_CBCLSEXTRA     (-20)
#define GCL_WNDPROC        (-24)
#define GCL_STYLE          (-26)
#define GCW_ATOM           (-32)
#define GCL_HICONSM        (-34)
#define GCLP_MENUNAME      GCL_MENUNAME
#define GCLP_HBRBACKGROUND GCL_HBRBACKGROUND
#define GCLP_HCURSOR       GCL_HCURSOR
#define GCLP_HICON         GCL_HICON
#define GCLP_HMODULE       GCL_HMODULE
#define GCLP_WNDPROC       GCL_WNDPROC
#define GCLP_HICONSM       GCL_HICONSM

#define WM_NULL        0x0000u
#ifndef WM_CREATE
#define WM_CREATE      0x0001u
#endif
#ifndef WM_DESTROY
#define WM_DESTROY     0x0002u
#endif
#ifndef WM_MOVE
#define WM_MOVE        0x0003u
#endif
#ifndef WM_SIZE
#define WM_SIZE        0x0005u
#endif
#ifndef WM_ACTIVATE
#define WM_ACTIVATE    0x0006u
#endif
#ifndef WM_SETFOCUS
#define WM_SETFOCUS    0x0007u
#endif
#ifndef WM_KILLFOCUS
#define WM_KILLFOCUS   0x0008u
#endif
#ifndef WM_ENABLE
#define WM_ENABLE      0x000Au
#endif
#ifndef WM_SETTEXT
#define WM_SETTEXT     0x000Cu
#endif
#ifndef WM_GETTEXT
#define WM_GETTEXT     0x000Du
#endif
#ifndef WM_GETTEXTLENGTH
#define WM_GETTEXTLENGTH 0x000Eu
#endif
#ifndef WM_PAINT
#define WM_PAINT       0x000Fu
#endif
#ifndef WM_ERASEBKGND
#define WM_ERASEBKGND  0x0014u
#endif
#ifndef WM_CLOSE
#define WM_CLOSE       0x0010u
#endif
#define WM_QUIT        0x0012u
#ifndef WM_SHOWWINDOW
#define WM_SHOWWINDOW  0x0018u
#endif
#ifndef WM_CANCELMODE
#define WM_CANCELMODE  0x001Fu
#endif
#ifndef WM_GETMINMAXINFO
#define WM_GETMINMAXINFO 0x0024u
#endif
#ifndef WM_NEXTDLGCTL
#define WM_NEXTDLGCTL  0x0028u
#endif
#define WM_DRAWITEM    0x002Bu
#define WM_MEASUREITEM 0x002Cu
#ifndef WM_WINDOWPOSCHANGING
#define WM_WINDOWPOSCHANGING 0x0046u
#endif
#ifndef WM_WINDOWPOSCHANGED
#define WM_WINDOWPOSCHANGED  0x0047u
#endif
#ifndef WM_NCCREATE
#define WM_NCCREATE    0x0081u
#endif
#ifndef WM_NCDESTROY
#define WM_NCDESTROY   0x0082u
#endif
#ifndef WM_NCHITTEST
#define WM_NCHITTEST   0x0084u
#endif
#ifndef WM_GETDLGCODE
#define WM_GETDLGCODE  0x0087u
#endif
#ifndef WM_NCMOUSEMOVE
#define WM_NCMOUSEMOVE 0x00A0u
#endif
#ifndef WM_NCLBUTTONDOWN
#define WM_NCLBUTTONDOWN 0x00A1u
#endif
#ifndef WM_NCLBUTTONUP
#define WM_NCLBUTTONUP   0x00A2u
#endif
#ifndef WM_NCRBUTTONDOWN
#define WM_NCRBUTTONDOWN 0x00A4u
#endif
#ifndef WM_NCRBUTTONUP
#define WM_NCRBUTTONUP   0x00A5u
#endif
#ifndef WM_KEYDOWN
#define WM_KEYDOWN     0x0100u
#endif
#ifndef WM_KEYUP
#define WM_KEYUP       0x0101u
#endif
#ifndef WM_CHAR
#define WM_CHAR        0x0102u
#endif
#ifndef WM_SYSKEYDOWN
#define WM_SYSKEYDOWN  0x0104u
#endif
#ifndef WM_SYSKEYUP
#define WM_SYSKEYUP    0x0105u
#endif
#ifndef WM_SYSCHAR
#define WM_SYSCHAR     0x0106u
#endif
#ifndef WM_INITDIALOG
#define WM_INITDIALOG  0x0110u
#endif
#ifndef WM_COMMAND
#define WM_COMMAND     0x0111u
#endif
#ifndef WM_SYSCOMMAND
#define WM_SYSCOMMAND  0x0112u
#endif
#define SC_SIZE       0xF000u
#define SC_MOVE       0xF010u
#define SC_MINIMIZE   0xF020u
#define SC_MAXIMIZE   0xF030u
#define SC_CLOSE      0xF060u
#define SC_RESTORE    0xF120u
#define SC_KEYMENU    0xF100u
#ifndef WM_TIMER
#define WM_TIMER       0x0113u
#endif
#ifndef WM_HSCROLL
#define WM_HSCROLL     0x0114u
#endif
#ifndef WM_VSCROLL
#define WM_VSCROLL     0x0115u
#endif
#ifndef WM_INITMENU
#define WM_INITMENU    0x0116u
#endif
#ifndef WM_INITMENUPOPUP
#define WM_INITMENUPOPUP 0x0117u
#endif
#ifndef WM_MENUSELECT
#define WM_MENUSELECT  0x011Fu
#endif
#define WM_UNINITMENUPOPUP 0x0125u
#ifndef WM_MOUSEMOVE
#define WM_MOUSEMOVE   0x0200u
#endif
#ifndef WM_LBUTTONDOWN
#define WM_LBUTTONDOWN 0x0201u
#endif
#ifndef WM_LBUTTONUP
#define WM_LBUTTONUP   0x0202u
#endif
#ifndef WM_RBUTTONDOWN
#define WM_RBUTTONDOWN 0x0204u
#endif
#ifndef WM_RBUTTONUP
#define WM_RBUTTONUP   0x0205u
#endif
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL  0x020Au
#endif
#ifndef WM_ENTERMENULOOP
#define WM_ENTERMENULOOP 0x0211u
#endif
#ifndef WM_EXITMENULOOP
#define WM_EXITMENULOOP  0x0212u
#endif
#ifndef WM_CAPTURECHANGED
#define WM_CAPTURECHANGED 0x0215u
#endif
#ifndef WM_ENTERSIZEMOVE
#define WM_ENTERSIZEMOVE 0x0231u
#endif
#ifndef WM_EXITSIZEMOVE
#define WM_EXITSIZEMOVE  0x0232u
#endif
#ifndef WM_SURFACE_DIRTY
#define WM_SURFACE_DIRTY 0x0310u
#endif
#ifndef WM_USER
#define WM_CHILDACTIVATE 0x0022u
#define WM_MDICREATE     0x0220u
#define WM_MDIDESTROY    0x0221u
#define WM_MDIACTIVATE   0x0222u
#define WM_MDIRESTORE    0x0223u
#define WM_MDINEXT       0x0224u
#define WM_MDIMAXIMIZE   0x0225u
#define WM_MDITILE       0x0226u
#define WM_MDICASCADE    0x0227u
#define WM_MDIICONARRANGE 0x0228u
#define WM_MDIGETACTIVE  0x0229u
#define WM_MDISETMENU    0x0230u
#define MDIS_ALLCHILDSTYLES 0x0001u
#define MDITILE_VERTICAL     0x0000u
#define MDITILE_HORIZONTAL   0x0001u
#define MDITILE_SKIPDISABLED 0x0002u
#define MDICASCADE_SKIPDISABLED 0x0002u

#define WM_USER        0x0400u
#endif
#ifndef WM_CHILDACTIVATE
#define WM_CHILDACTIVATE 0x0022u
#endif
#ifndef WM_MDICREATE
#define WM_MDICREATE     0x0220u
#endif
#ifndef WM_MDIDESTROY
#define WM_MDIDESTROY    0x0221u
#endif
#ifndef WM_MDIACTIVATE
#define WM_MDIACTIVATE   0x0222u
#endif
#ifndef WM_MDIRESTORE
#define WM_MDIRESTORE    0x0223u
#endif
#ifndef WM_MDINEXT
#define WM_MDINEXT       0x0224u
#endif
#ifndef WM_MDIMAXIMIZE
#define WM_MDIMAXIMIZE   0x0225u
#endif
#ifndef WM_MDITILE
#define WM_MDITILE       0x0226u
#endif
#ifndef WM_MDICASCADE
#define WM_MDICASCADE    0x0227u
#endif
#ifndef WM_MDIICONARRANGE
#define WM_MDIICONARRANGE 0x0228u
#endif
#ifndef WM_MDIGETACTIVE
#define WM_MDIGETACTIVE  0x0229u
#endif
#ifndef WM_MDISETMENU
#define WM_MDISETMENU    0x0230u
#endif
#ifndef MDIS_ALLCHILDSTYLES
#define MDIS_ALLCHILDSTYLES 0x0001u
#endif
#ifndef MDITILE_VERTICAL
#define MDITILE_VERTICAL     0x0000u
#endif
#ifndef MDITILE_HORIZONTAL
#define MDITILE_HORIZONTAL   0x0001u
#endif
#ifndef MDITILE_SKIPDISABLED
#define MDITILE_SKIPDISABLED 0x0002u
#endif
#ifndef MDICASCADE_SKIPDISABLED
#define MDICASCADE_SKIPDISABLED 0x0002u
#endif

#define DM_GETDEFID    (WM_USER + 0)
#define DM_SETDEFID    (WM_USER + 1)

#ifndef VK_TAB
#define VK_TAB     0x09u
#endif
#ifndef VK_RETURN
#define VK_RETURN  0x0Du
#endif
#ifndef VK_ESCAPE
#define VK_ESCAPE  0x1Bu
#endif
#ifndef VK_SPACE
#define VK_SPACE   0x20u
#endif
#ifndef VK_PRIOR
#define VK_PRIOR   0x21u
#endif
#ifndef VK_NEXT
#define VK_NEXT    0x22u
#endif
#ifndef VK_END
#define VK_END     0x23u
#endif
#ifndef VK_HOME
#define VK_HOME    0x24u
#endif
#ifndef VK_LEFT
#define VK_LEFT    0x25u
#endif
#ifndef VK_UP
#define VK_UP      0x26u
#endif
#ifndef VK_RIGHT
#define VK_RIGHT   0x27u
#endif
#ifndef VK_DOWN
#define VK_DOWN    0x28u
#endif


#define IDOK      1
#define IDCANCEL  2
#define DC_HASDEFID 0x534Bu

#define DLGC_WANTARROWS      0x0001u
#define DLGC_WANTTAB         0x0002u
#define DLGC_WANTALLKEYS     0x0004u
#define DLGC_WANTMESSAGE     0x0004u
#define DLGC_HASSETSEL       0x0008u
#define DLGC_DEFPUSHBUTTON   0x0010u
#define DLGC_UNDEFPUSHBUTTON 0x0020u
#define DLGC_RADIOBUTTON     0x0040u
#define DLGC_WANTCHARS       0x0080u
#define DLGC_STATIC          0x0100u
#define DLGC_BUTTON          0x2000u

#define SIZE_RESTORED  0u
#define SIZE_MINIMIZED 1u
#define SIZE_MAXIMIZED 2u

#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffffu))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffffu))
#define MAKEWPARAM(l,h) ((WPARAM)(((WORD)(l)) | ((DWORD_PTR)((WORD)(h)) << 16)))
#define MAKELPARAM(l,h) ((LPARAM)(((WORD)(l)) | ((DWORD_PTR)((WORD)(h)) << 16)))
#define MAKELRESULT(l,h) ((LRESULT)(((WORD)(l)) | ((DWORD_PTR)((WORD)(h)) << 16)))
#define GET_X_LPARAM(lp) ((int)(SHORT)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(SHORT)HIWORD(lp))
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))
#define GET_KEYSTATE_WPARAM(wp)    LOWORD(wp)
#define MK_LBUTTON  0x0001u
#define MK_RBUTTON  0x0002u
#define MK_SHIFT    0x0004u
#define MK_CONTROL  0x0008u
#define MK_MBUTTON  0x0010u
#define WHEEL_DELTA 120

#define CS_VREDRAW  0x0001u
#define CS_HREDRAW  0x0002u

#define DS_FIXEDSYS      0x00000008u
#define DS_SETFONT       0x00000040u
#define DS_MODALFRAME    0x00000080u
#define DS_SHELLFONT     (DS_SETFONT | DS_FIXEDSYS)

#define BS_PUSHBUTTON      0x00000000u
#define BS_DEFPUSHBUTTON   0x00000001u
#define BS_CHECKBOX        0x00000002u
#define BS_AUTOCHECKBOX    0x00000003u
#define BS_RADIOBUTTON     0x00000004u
#define BS_3STATE          0x00000005u
#define BS_AUTO3STATE      0x00000006u
#define BS_GROUPBOX        0x00000007u
#define BS_USERBUTTON      0x00000008u
#define BS_AUTORADIOBUTTON 0x00000009u
#define BS_PUSHBOX         0x0000000Au
#define BS_OWNERDRAW       0x0000000Bu
#define BS_TYPEMASK        0x0000000Fu
#define BS_LEFTTEXT        0x00000020u
#define BS_TEXT            0x00000000u
#define BS_ICON            0x00000040u
#define BS_BITMAP          0x00000080u
#define BS_LEFT            0x00000100u
#define BS_RIGHT           0x00000200u
#define BS_CENTER          0x00000300u
#define BS_TOP             0x00000400u
#define BS_BOTTOM          0x00000800u
#define BS_VCENTER         0x00000C00u
#define BS_PUSHLIKE        0x00001000u
#define BS_MULTILINE       0x00002000u
#define BS_NOTIFY          0x00004000u
#define BS_FLAT            0x00008000u

#define BN_CLICKED 0
#define BN_PAINT 1
#define BN_HILITE 2
#define BN_UNHILITE 3
#define BN_DISABLE 4
#define BN_DOUBLECLICKED 5
#define BN_SETFOCUS 6
#define BN_KILLFOCUS 7

#define BM_GETCHECK 0x00F0u
#define BM_SETCHECK 0x00F1u
#define BM_GETSTATE 0x00F2u
#define BM_SETSTATE 0x00F3u
#define BM_SETSTYLE 0x00F4u
#define BM_CLICK    0x00F5u
#define BST_UNCHECKED 0x0000u
#define BST_CHECKED   0x0001u
#define BST_INDETERMINATE 0x0002u
#define BST_PUSHED    0x0004u
#define BST_FOCUS     0x0008u

#define SS_LEFT             0x00000000u
#define SS_CENTER           0x00000001u
#define SS_RIGHT            0x00000002u
#define SS_ICON             0x00000003u
#define SS_BLACKRECT        0x00000004u
#define SS_GRAYRECT         0x00000005u
#define SS_WHITERECT        0x00000006u
#define SS_BLACKFRAME       0x00000007u
#define SS_GRAYFRAME        0x00000008u
#define SS_WHITEFRAME       0x00000009u
#define SS_USERITEM         0x0000000Au
#define SS_SIMPLE           0x0000000Bu
#define SS_LEFTNOWORDWRAP   0x0000000Cu
#define SS_OWNERDRAW        0x0000000Du
#define SS_BITMAP           0x0000000Eu
#define SS_ENHMETAFILE      0x0000000Fu
#define SS_ETCHEDHORZ       0x00000010u
#define SS_ETCHEDVERT       0x00000011u
#define SS_ETCHEDFRAME      0x00000012u
#define SS_TYPEMASK         0x0000001Fu
#define SS_NOPREFIX         0x00000080u
#define SS_NOTIFY           0x00000100u
#define SS_CENTERIMAGE      0x00000200u

#define ES_LEFT             0x00000000u
#define ES_CENTER           0x00000001u
#define ES_RIGHT            0x00000002u
#define ES_MULTILINE        0x00000004u
#define ES_UPPERCASE        0x00000008u
#define ES_LOWERCASE        0x00000010u
#define ES_PASSWORD         0x00000020u
#define ES_AUTOVSCROLL      0x00000040u
#define ES_AUTOHSCROLL      0x00000080u
#define ES_NOHIDESEL        0x00000100u
#define ES_OEMCONVERT       0x00000400u
#define ES_READONLY         0x00000800u
#define ES_WANTRETURN       0x00001000u
#define ES_NUMBER           0x00002000u

#define EN_SETFOCUS         0x0100u
#define EN_KILLFOCUS        0x0200u
#define EN_CHANGE           0x0300u
#define EN_UPDATE           0x0400u
#define EN_ERRSPACE         0x0500u
#define EN_MAXTEXT          0x0501u
#define EN_HSCROLL          0x0601u
#define EN_VSCROLL          0x0602u

#define EM_GETSEL           0x00B0u
#define EM_SETSEL           0x00B1u
#define EM_GETRECT          0x00B2u
#define EM_SETRECT          0x00B3u
#define EM_SETRECTNP        0x00B4u
#define EM_SCROLL           0x00B5u
#define EM_LINESCROLL       0x00B6u
#define EM_SCROLLCARET      0x00B7u
#define EM_GETMODIFY        0x00B8u
#define EM_SETMODIFY        0x00B9u
#define EM_GETLINECOUNT     0x00BAu
#define EM_LINEINDEX        0x00BBu
#define EM_SETHANDLE        0x00BCu
#define EM_GETHANDLE        0x00BDu
#define EM_GETTHUMB         0x00BEu
#define EM_LINELENGTH       0x00C1u
#define EM_REPLACESEL       0x00C2u
#define EM_GETLINE          0x00C4u
#define EM_LIMITTEXT        0x00C5u
#define EM_CANUNDO          0x00C6u
#define EM_UNDO             0x00C7u
#define EM_FMTLINES         0x00C8u
#define EM_LINEFROMCHAR     0x00C9u
#define EM_SETTABSTOPS      0x00CBu
#define EM_SETPASSWORDCHAR  0x00CCu
#define EM_EMPTYUNDOBUFFER  0x00CDu
#define EM_GETFIRSTVISIBLELINE 0x00CEu
#define EM_SETREADONLY      0x00CFu
#define EM_SETWORDBREAKPROC 0x00D0u
#define EM_GETWORDBREAKPROC 0x00D1u
#define EM_GETPASSWORDCHAR  0x00D2u
#define EM_SETMARGINS       0x00D3u
#define EM_GETMARGINS       0x00D4u

#ifndef LBS_NOTIFY
#define LBS_NOTIFY        0x0001u
#endif
#ifndef LBS_SORT
#define LBS_SORT          0x0002u
#endif
#ifndef LBS_NOREDRAW
#define LBS_NOREDRAW      0x0004u
#endif
#ifndef LBS_MULTIPLESEL
#define LBS_MULTIPLESEL   0x0008u
#endif
#ifndef LBS_OWNERDRAWFIXED
#define LBS_OWNERDRAWFIXED 0x0010u
#endif
#define LBS_OWNERDRAWVARIABLE 0x0020u
#ifndef LBS_HASSTRINGS
#define LBS_HASSTRINGS    0x0040u
#endif
#ifndef LBS_USETABSTOPS
#define LBS_USETABSTOPS   0x0080u
#endif
#ifndef LBS_NOINTEGRALHEIGHT
#define LBS_NOINTEGRALHEIGHT 0x0100u
#endif
#ifndef LBS_MULTICOLUMN
#define LBS_MULTICOLUMN   0x0200u
#endif
#ifndef LBS_WANTKEYBOARDINPUT
#define LBS_WANTKEYBOARDINPUT 0x0400u
#endif
#ifndef LBS_EXTENDEDSEL
#define LBS_EXTENDEDSEL   0x0800u
#endif
#define LBS_DISABLENOSCROLL 0x1000u
#define LBS_NODATA        0x2000u
#define LBS_NOSEL         0x4000u
#define LBN_ERRSPACE     (-2)
#define LBN_SELCHANGE     1
#define LBN_DBLCLK        2
#define LBN_SELCANCEL     3
#define LBN_SETFOCUS      4
#define LBN_KILLFOCUS     5
#define LB_ERR          (-1)
#define LB_ERRSPACE     (-2)
#define LB_ADDSTRING      0x0180u
#define LB_INSERTSTRING   0x0181u
#define LB_DELETESTRING   0x0182u
#define LB_RESETCONTENT   0x0184u
#define LB_SETSEL         0x0185u
#define LB_SETCURSEL      0x0186u
#define LB_GETSEL         0x0187u
#define LB_GETCURSEL      0x0188u
#define LB_GETTEXT        0x0189u
#define LB_GETTEXTLEN     0x018Au
#define LB_GETCOUNT       0x018Bu
#define LB_GETSELCOUNT    0x0190u
#define LB_GETSELITEMS    0x0191u
#define LB_SETTABSTOPS    0x0192u
#define LB_GETHORIZONTALEXTENT 0x0193u
#define LB_SETHORIZONTALEXTENT 0x0194u
#define LB_SETCOLUMNWIDTH 0x0195u
#define LB_SELECTSTRING   0x018Cu
#define LB_DIR            0x018Du
#define LB_GETTOPINDEX    0x018Eu
#define LB_FINDSTRING     0x018Fu
#define LB_SETTOPINDEX    0x0197u
#define LB_GETITEMRECT    0x0198u
#define LB_GETITEMDATA    0x0199u
#define LB_SETITEMDATA    0x019Au
#define LB_SELITEMRANGE   0x019Bu
#define LB_SETANCHORINDEX 0x019Cu
#define LB_GETANCHORINDEX 0x019Du
#define LB_SETCARETINDEX  0x019Eu
#define LB_GETCARETINDEX  0x019Fu
#define LB_SETITEMHEIGHT  0x01A0u
#define LB_GETITEMHEIGHT  0x01A1u
#define LB_FINDSTRINGEXACT 0x01A2u

#define SB_HORZ 0
#define SB_VERT 1
#define SB_CTL  2
#define SB_BOTH 3
#define SB_LINEUP 0
#define SB_LINELEFT 0
#define SB_LINEDOWN 1
#define SB_LINERIGHT 1
#define SB_PAGEUP 2
#define SB_PAGELEFT 2
#define SB_PAGEDOWN 3
#define SB_PAGERIGHT 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SB_TOP 6
#define SB_LEFT 6
#define SB_BOTTOM 7
#define SB_RIGHT 7
#define SB_ENDSCROLL 8
#define SBS_HORZ          0x0000L
#define SBS_VERT          0x0001L
#define SBS_TOPALIGN      0x0002L
#define SBS_LEFTALIGN     0x0002L
#define SBS_BOTTOMALIGN   0x0004L
#define SBS_RIGHTALIGN    0x0004L
#define SBS_SIZEBOXTOPLEFTALIGN 0x0002L
#define SBS_SIZEBOXBOTTOMRIGHTALIGN 0x0004L
#define SBS_SIZEBOX       0x0008L
#define SBM_SETPOS        0x00E0u
#define SBM_GETPOS        0x00E1u
#define SBM_SETRANGE      0x00E2u
#define SBM_GETRANGE      0x00E3u
#define SBM_ENABLE_ARROWS 0x00E4u
#define SBM_SETRANGEREDRAW 0x00E6u
#define SIF_RANGE           0x0001u
#define SIF_PAGE            0x0002u
#define SIF_POS             0x0004u
#define SIF_DISABLENOSCROLL 0x0008u
#define SIF_TRACKPOS        0x0010u
#define SIF_ALL             (SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS)
#define ESB_ENABLE_BOTH     0x0000u
#define ESB_DISABLE_BOTH    0x0003u
#define ESB_DISABLE_LEFT    0x0001u
#define ESB_DISABLE_RIGHT   0x0002u
#define ESB_DISABLE_UP      0x0001u
#define ESB_DISABLE_DOWN    0x0002u
#define ESB_DISABLE_LTUP    ESB_DISABLE_LEFT
#define ESB_DISABLE_RTDN    ESB_DISABLE_RIGHT

#define CBS_SIMPLE        0x0001L
#define CBS_DROPDOWN      0x0002L
#define CBS_DROPDOWNLIST  0x0003L
#define CBS_OWNERDRAWFIXED 0x0010L
#define CBS_OWNERDRAWVARIABLE 0x0020L
#define CBS_AUTOHSCROLL   0x0040L
#define CBS_OEMCONVERT    0x0080L
#define CBS_SORT          0x0100L
#define CBS_HASSTRINGS    0x0200L
#define CBS_NOINTEGRALHEIGHT 0x0400L
#define CBS_DISABLENOSCROLL 0x0800L
#define CBS_UPPERCASE     0x2000L
#define CBS_LOWERCASE     0x4000L
#define CB_ERR           (-1)
#define CB_ERRSPACE      (-2)
#define CB_GETEDITSEL     0x0140u
#define CB_LIMITTEXT      0x0141u
#define CB_SETEDITSEL     0x0142u
#define CB_ADDSTRING      0x0143u
#define CB_DELETESTRING   0x0144u
#define CB_DIR            0x0145u
#define CB_GETCOUNT       0x0146u
#define CB_GETCURSEL      0x0147u
#define CB_GETLBTEXT      0x0148u
#define CB_GETLBTEXTLEN   0x0149u
#define CB_INSERTSTRING   0x014Au
#define CB_RESETCONTENT   0x014Bu
#define CB_FINDSTRING     0x014Cu
#define CB_SELECTSTRING   0x014Du
#define CB_SETCURSEL      0x014Eu
#define CB_SHOWDROPDOWN   0x014Fu
#define CB_GETITEMDATA    0x0150u
#define CB_SETITEMDATA    0x0151u
#define CB_GETDROPPEDCONTROLRECT 0x0152u
#define CB_SETITEMHEIGHT  0x0153u
#define CB_GETITEMHEIGHT  0x0154u
#define CB_SETEXTENDEDUI  0x0155u
#define CB_GETEXTENDEDUI  0x0156u
#define CB_GETDROPPEDSTATE 0x0157u
#define CB_FINDSTRINGEXACT 0x0158u
#define CB_SETLOCALE      0x0159u
#define CB_GETLOCALE      0x015Au
#define CB_GETTOPINDEX    0x015Bu
#define CB_SETTOPINDEX    0x015Cu
#define CB_GETHORIZONTALEXTENT 0x015Du
#define CB_SETHORIZONTALEXTENT 0x015Eu
#define CB_GETDROPPEDWIDTH 0x015Fu
#define CB_SETDROPPEDWIDTH 0x0160u

#define CBN_ERRSPACE      (-1)
#define CBN_SELCHANGE     1
#define CBN_DBLCLK        2
#define CBN_SETFOCUS      3
#define CBN_KILLFOCUS     4
#define CBN_EDITCHANGE    5
#define CBN_EDITUPDATE    6
#define CBN_DROPDOWN      7
#define CBN_CLOSEUP       8
#define CBN_SELENDOK      9
#define CBN_SELENDCANCEL  10

#define HTERROR        (-2)
#define HTTRANSPARENT  (-1)
#define HTNOWHERE       0
#define HTCLIENT        1
#define HTCAPTION       2
#define HTSYSMENU       3
#define HTGROWBOX       4
#define HTSIZE          HTGROWBOX
#define HTMENU          5
#define HTHSCROLL       6
#define HTVSCROLL       7
#define HTMINBUTTON     8
#define HTMAXBUTTON     9
#define HTLEFT         10
#define HTRIGHT        11
#define HTTOP          12
#define HTTOPLEFT      13
#define HTTOPRIGHT     14
#define HTBOTTOM       15
#define HTBOTTOMLEFT   16
#define HTBOTTOMRIGHT  17
#define HTCLOSE        20

#define CF_TEXT 1u
#define MF_INSERT       0x0000u
#define MF_CHANGE       0x0080u
#define MF_APPEND       0x0100u
#define MF_DELETE       0x0200u
#define MF_REMOVE       0x1000u
#define MF_BYCOMMAND    0x0000u
#define MF_BYPOSITION   0x0400u
#define MF_STRING       0x0000u
#define MF_BITMAP       0x0004u
#define MF_OWNERDRAW    0x0100u
#define MF_SEPARATOR    0x0800u
#define MF_POPUP        0x0010u
#define MF_CHECKED      0x0008u
#define MF_UNCHECKED    0x0000u
#define MF_ENABLED      0x0000u
#define MF_GRAYED       0x0001u
#define MF_DISABLED     0x0002u
#define TPM_RECURSE     0x0001u
#define TPM_LEFTBUTTON  0x0000u
#define TPM_RIGHTBUTTON 0x0002u
#define TPM_LEFTALIGN   0x0000u
#define TPM_CENTERALIGN 0x0004u
#define TPM_RIGHTALIGN  0x0008u
#define TPM_TOPALIGN    0x0000u
#define TPM_VCENTERALIGN 0x0010u
#define TPM_BOTTOMALIGN 0x0020u
#define TPM_HORIZONTAL  0x0000u
#define TPM_VERTICAL    0x0040u
#define TPM_NONOTIFY    0x0080u
#define TPM_RETURNCMD   0x0100u
#define TPM_HORPOSANIMATION 0x0400u
#define TPM_HORNEGANIMATION 0x0800u
#define TPM_VERPOSANIMATION 0x1000u
#define TPM_VERNEGANIMATION 0x2000u
#define TPM_NOANIMATION 0x4000u
#define TPM_LAYOUTRTL   0x8000u
#define TPM_WORKAREA    0x10000u
#define MIIM_STATE      0x00000001u
#define MIIM_ID         0x00000002u
#define MIIM_SUBMENU    0x00000004u
#define MIIM_CHECKMARKS 0x00000008u
#define MIIM_TYPE       0x00000010u
#define MIIM_DATA       0x00000020u
#define MIIM_STRING     0x00000040u
#define MIIM_BITMAP     0x00000080u
#define MIIM_FTYPE      0x00000100u
#define MFT_STRING      MF_STRING
#define MFT_BITMAP      MF_BITMAP
#define MFT_SEPARATOR   MF_SEPARATOR
#define MFT_OWNERDRAW   MF_OWNERDRAW
#define MFS_GRAYED      MF_GRAYED
#define MFS_DISABLED    MFS_GRAYED
#define MFS_CHECKED     MF_CHECKED
#define MFS_ENABLED     MF_ENABLED
#define MFS_UNCHECKED   MF_UNCHECKED

#define ODT_MENU        1u
#define ODT_LISTBOX     2u
#define ODT_COMBOBOX    3u
#define ODT_BUTTON      4u
#define ODA_DRAWENTIRE  0x0001u
#define ODA_SELECT      0x0002u
#define ODA_FOCUS       0x0004u
#define ODS_SELECTED    0x0001u
#define ODS_GRAYED      0x0002u
#define ODS_DISABLED    0x0004u
#define ODS_CHECKED     0x0008u
#define ODS_FOCUS       0x0010u
#define FVIRTKEY 0x01u
#define FSHIFT   0x04u
#define FCONTROL 0x08u
#define FALT     0x10u

typedef struct tagMSG {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
} MSG, *PMSG, *LPMSG;

typedef LRESULT (CALLBACK *WNDPROC)(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
typedef INT_PTR (CALLBACK *DLGPROC)(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);

/* v153: dialog-specific window extra offsets.  These are offsets into
   the DLGWINDOWEXTRA bytes owned by #32770/DefDlgProc windows, distinct
   from GWLP_WNDPROC/GWLP_USERDATA. */
#define DWLP_MSGRESULT  0
#define DWLP_DLGPROC    (DWLP_MSGRESULT + (int)sizeof(LRESULT))
#define DWLP_USER       (DWLP_DLGPROC + (int)sizeof(DLGPROC))
#define DLGWINDOWEXTRA  (DWLP_USER + (int)sizeof(LONG_PTR))
#define DWL_MSGRESULT   0
#define DWL_DLGPROC     DWLP_DLGPROC
#define DWL_USER        DWLP_USER
typedef BOOL (CALLBACK *WNDENUMPROC)(HWND hWnd, LPARAM lParam);
typedef void (CALLBACK *TIMERPROC)(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);

#ifndef MYOS_PACKED
#if defined(__GNUC__) || defined(__clang__)
#define MYOS_PACKED __attribute__((packed))
#else
#define MYOS_PACKED
#endif
#endif

typedef struct MYOS_PACKED tagDLGTEMPLATE {
    DWORD style;
    DWORD dwExtendedStyle;
    WORD  cdit;
    short x;
    short y;
    short cx;
    short cy;
} DLGTEMPLATE, *LPDLGTEMPLATE;
typedef const DLGTEMPLATE* LPCDLGTEMPLATEA;
typedef const DLGTEMPLATE* LPCDLGTEMPLATE;

typedef struct MYOS_PACKED tagDLGITEMTEMPLATE {
    DWORD style;
    DWORD dwExtendedStyle;
    short x;
    short y;
    short cx;
    short cy;
    WORD  id;
} DLGITEMTEMPLATE, *LPDLGITEMTEMPLATE;
typedef const DLGITEMTEMPLATE* LPCDLGITEMTEMPLATEA;

typedef struct MYOS_PACKED tagDLGTEMPLATEEX {
    WORD  dlgVer;
    WORD  signature;
    DWORD helpID;
    DWORD exStyle;
    DWORD style;
    WORD  cDlgItems;
    short x;
    short y;
    short cx;
    short cy;
} DLGTEMPLATEEX, *LPDLGTEMPLATEEX;
typedef const DLGTEMPLATEEX* LPCDLGTEMPLATEEXA;

typedef struct MYOS_PACKED tagDLGITEMTEMPLATEEX {
    DWORD helpID;
    DWORD exStyle;
    DWORD style;
    short x;
    short y;
    short cx;
    short cy;
    DWORD id;
} DLGITEMTEMPLATEEX, *LPDLGITEMTEMPLATEEX;
typedef const DLGITEMTEMPLATEEX* LPCDLGITEMTEMPLATEEXA;

typedef struct tagWNDCLASSEXA {
    UINT      cbSize;
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra;
    int       cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    LPCSTR    lpszMenuName;
    LPCSTR    lpszClassName;
    HICON     hIconSm;
} WNDCLASSEXA, *PWNDCLASSEXA, *LPWNDCLASSEXA;

typedef struct tagSCROLLINFO {
    UINT cbSize;
    UINT fMask;
    int  nMin;
    int  nMax;
    UINT nPage;
    int  nPos;
    int  nTrackPos;
} SCROLLINFO, *LPSCROLLINFO;
typedef const SCROLLINFO *LPCSCROLLINFO;

typedef struct tagCREATESTRUCTA {
    LPVOID    lpCreateParams;
    HINSTANCE hInstance;
    HMENU     hMenu;
    HWND      hwndParent;
    int       cy;
    int       cx;
    int       y;
    int       x;
    LONG      style;
    LPCSTR    lpszName;
    LPCSTR    lpszClass;
    DWORD     dwExStyle;
} CREATESTRUCTA, *LPCREATESTRUCTA;

#ifndef MYOS_WINDOWPOS_DEFINED
#define MYOS_WINDOWPOS_DEFINED
typedef struct tagWINDOWPOS {
    HWND hwnd;
    HWND hwndInsertAfter;
    int  x;
    int  y;
    int  cx;
    int  cy;
    UINT flags;
} WINDOWPOS, *PWINDOWPOS, *LPWINDOWPOS;
#endif

#ifndef MYOS_MINMAXINFO_DEFINED
#define MYOS_MINMAXINFO_DEFINED
typedef struct tagMINMAXINFO {
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
} MINMAXINFO, *PMINMAXINFO, *LPMINMAXINFO;
#endif

#ifndef MYOS_WINDOWPLACEMENT_DEFINED
#define MYOS_WINDOWPLACEMENT_DEFINED
#define WPF_SETMINPOSITION 0x0001u
typedef struct tagWINDOWPLACEMENT {
    UINT  length;
    UINT  flags;
    UINT  showCmd;
    POINT ptMinPosition;
    POINT ptMaxPosition;
    RECT  rcNormalPosition;
} WINDOWPLACEMENT, *PWINDOWPLACEMENT, *LPWINDOWPLACEMENT;
#endif

typedef struct tagMENUITEMINFOA {
    UINT      cbSize;
    UINT      fMask;
    UINT      fType;
    UINT      fState;
    UINT      wID;
    HMENU     hSubMenu;
    HBITMAP   hbmpChecked;
    HBITMAP   hbmpUnchecked;
    ULONG_PTR dwItemData;
    LPSTR     dwTypeData;
    UINT      cch;
    HBITMAP   hbmpItem;
} MENUITEMINFOA, *LPMENUITEMINFOA;

#ifndef MYOS_TPMPARAMS_DEFINED
#define MYOS_TPMPARAMS_DEFINED
typedef struct tagTPMPARAMS {
    UINT cbSize;
    RECT rcExclude;
} TPMPARAMS, *PTPMPARAMS, *LPTPMPARAMS;
#endif

typedef struct tagCLIENTCREATESTRUCT {
    HMENU hWindowMenu;
    UINT  idFirstChild;
} CLIENTCREATESTRUCT, *LPCLIENTCREATESTRUCT;

typedef struct tagMDICREATESTRUCTA {
    LPCSTR szClass;
    LPCSTR szTitle;
    HANDLE hOwner;
    int    x;
    int    y;
    int    cx;
    int    cy;
    DWORD  style;
    LPARAM lParam;
} MDICREATESTRUCTA, *LPMDICREATESTRUCTA;

typedef struct tagACCEL {
    BYTE fVirt;
    WORD key;
    WORD cmd;
} ACCEL, *LPACCEL;

typedef struct tagMEASUREITEMSTRUCT {
    UINT      CtlType;
    UINT      CtlID;
    UINT      itemID;
    UINT      itemWidth;
    UINT      itemHeight;
    ULONG_PTR itemData;
} MEASUREITEMSTRUCT, *PMEASUREITEMSTRUCT, *LPMEASUREITEMSTRUCT;

typedef struct tagDRAWITEMSTRUCT {
    UINT      CtlType;
    UINT      CtlID;
    UINT      itemID;
    UINT      itemAction;
    UINT      itemState;
    HWND      hwndItem;
    HDC       hDC;
    RECT      rcItem;
    ULONG_PTR itemData;
} DRAWITEMSTRUCT, *PDRAWITEMSTRUCT, *LPDRAWITEMSTRUCT;

ATOM    WINAPI RegisterClassExA(const WNDCLASSEXA* lpWndClass);
ATOM    WINAPI UnregisterClassA(LPCSTR lpClassName, HINSTANCE hInstance);
LONG_PTR WINAPI GetWindowLongPtrA(HWND hWnd, int nIndex);
LONG_PTR WINAPI SetWindowLongPtrA(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
LONG    WINAPI GetWindowLongA(HWND hWnd, int nIndex);
LONG    WINAPI SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong);
LONG_PTR WINAPI GetClassLongPtrA(HWND hWnd, int nIndex);
LONG_PTR WINAPI SetClassLongPtrA(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
LONG    WINAPI GetClassLongA(HWND hWnd, int nIndex);
LONG    WINAPI SetClassLongA(HWND hWnd, int nIndex, LONG dwNewLong);
LRESULT WINAPI CallWindowProcA(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
HWND    WINAPI CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName,
                               DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
                               HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
BOOL    WINAPI DestroyWindow(HWND hWnd);
BOOL    WINAPI PostMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI SendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL    WINAPI SendMessageTimeoutA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam,
                                   UINT fuFlags, UINT uTimeout, PDWORD_PTR lpdwResult);
BOOL    WINAPI PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
BOOL    WINAPI GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
UINT_PTR WINAPI SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc);
BOOL    WINAPI KillTimer(HWND hWnd, UINT_PTR uIDEvent);
BOOL    WINAPI TranslateMessage(const MSG* lpMsg);
LRESULT WINAPI DispatchMessageA(const MSG* lpMsg);
LRESULT WINAPI DefWindowProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI DefDlgProcA(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI DefFrameProcA(HWND hWnd, HWND hWndMDIClient, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI DefMDIChildProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL    WINAPI ShowWindow(HWND hWnd, int nCmdShow);
BOOL    WINAPI IsWindow(HWND hWnd);
BOOL    WINAPI IsWindowVisible(HWND hWnd);
BOOL    WINAPI GetUpdateRect(HWND hWnd, LPRECT lpRect, BOOL bErase);
BOOL    WINAPI InvalidateRgn(HWND hWnd, HRGN hRgn, BOOL bErase);
BOOL    WINAPI ValidateRgn(HWND hWnd, HRGN hRgn);
int     WINAPI GetUpdateRgn(HWND hWnd, HRGN hRgn, BOOL bErase);
BOOL    WINAPI UpdateWindow(HWND hWnd);
BOOL    WINAPI RedrawWindow(HWND hWnd, const RECT* lprcUpdate, HRGN hrgnUpdate, UINT flags);
HWND    WINAPI GetForegroundWindow(void);
HWND    WINAPI GetDesktopWindow(void);
BOOL    WINAPI SetForegroundWindow(HWND hWnd);
HWND    WINAPI SetFocus(HWND hWnd);
HWND    WINAPI GetFocus(void);
HWND    WINAPI GetNextDlgTabItem(HWND hDlg, HWND hCtl, BOOL bPrevious);
HWND    WINAPI GetNextDlgGroupItem(HWND hDlg, HWND hCtl, BOOL bPrevious);
BOOL    WINAPI IsDialogMessageA(HWND hDlg, LPMSG lpMsg);
BOOL    WINAPI IsDialogMessageW(HWND hDlg, LPMSG lpMsg);
BOOL    WINAPI MapDialogRect(HWND hDlg, LPRECT lpRect);
INT_PTR WINAPI DialogBoxIndirectParamA(HINSTANCE hInstance, LPCDLGTEMPLATEA hDialogTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
HWND    WINAPI CreateDialogIndirectParamA(HINSTANCE hInstance, LPCDLGTEMPLATEA lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
INT_PTR WINAPI DialogBoxParamA(HINSTANCE hInstance, LPCSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
HWND    WINAPI CreateDialogParamA(HINSTANCE hInstance, LPCSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
BOOL    WINAPI EndDialog(HWND hDlg, INT_PTR nResult);
BOOL    WINAPI GetWindowRect(HWND hWnd, LPRECT lpRect);
BOOL    WINAPI GetClientRect(HWND hWnd, LPRECT lpRect);
BOOL    WINAPI ScreenToClient(HWND hWnd, LPPOINT lpPoint);
BOOL    WINAPI ClientToScreen(HWND hWnd, LPPOINT lpPoint);
int     WINAPI MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints);
BOOL    WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
BOOL    WINAPI MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);
BOOL    WINAPI ScrollWindow(HWND hWnd, int XAmount, int YAmount, const RECT* lpRect, const RECT* lpClipRect);
int     WINAPI ScrollWindowEx(HWND hWnd, int dx, int dy, const RECT* prcScroll, const RECT* prcClip, HRGN hrgnUpdate, LPRECT prcUpdate, UINT flags);
HDWP    WINAPI BeginDeferWindowPos(int nNumWindows);
HDWP    WINAPI DeferWindowPos(HDWP hWinPosInfo, HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy, UINT uFlags);
BOOL    WINAPI EndDeferWindowPos(HDWP hWinPosInfo);
BOOL    WINAPI GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT* lpwndpl);
BOOL    WINAPI SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT* lpwndpl);
int     WINAPI GetClassNameA(HWND hWnd, LPSTR lpClassName, int nMaxCount);
int     WINAPI GetWindowTextA(HWND hWnd, LPSTR lpString, int nMaxCount);
BOOL    WINAPI SetWindowTextA(HWND hWnd, LPCSTR lpString);
int     WINAPI GetWindowTextLengthA(HWND hWnd);
HWND    WINAPI GetDlgItem(HWND hDlg, int nIDDlgItem);
LRESULT WINAPI SendDlgItemMessageA(HWND hDlg, int nIDDlgItem, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL    WINAPI SetDlgItemTextA(HWND hDlg, int nIDDlgItem, LPCSTR lpString);
UINT    WINAPI GetDlgItemTextA(HWND hDlg, int nIDDlgItem, LPSTR lpString, int cchMax);
UINT    WINAPI GetDlgItemInt(HWND hDlg, int nIDDlgItem, BOOL* lpTranslated, BOOL bSigned);
BOOL    WINAPI SetDlgItemInt(HWND hDlg, int nIDDlgItem, UINT uValue, BOOL bSigned);
BOOL    WINAPI EnableWindow(HWND hWnd, BOOL bEnable);
BOOL    WINAPI IsWindowEnabled(HWND hWnd);
BOOL    WINAPI CheckDlgButton(HWND hDlg, int nIDButton, UINT uCheck);
BOOL    WINAPI CheckRadioButton(HWND hDlg, int nIDFirstButton, int nIDLastButton, int nIDCheckButton);
UINT    WINAPI IsDlgButtonChecked(HWND hDlg, int nIDButton);
int     WINAPI SetScrollPos(HWND hWnd, int nBar, int nPos, BOOL bRedraw);
int     WINAPI GetScrollPos(HWND hWnd, int nBar);
BOOL    WINAPI SetScrollRange(HWND hWnd, int nBar, int nMinPos, int nMaxPos, BOOL bRedraw);
BOOL    WINAPI GetScrollRange(HWND hWnd, int nBar, LPINT lpMinPos, LPINT lpMaxPos);
int     WINAPI SetScrollInfo(HWND hWnd, int nBar, LPCSCROLLINFO lpsi, BOOL redraw);
BOOL    WINAPI GetScrollInfo(HWND hWnd, int nBar, LPSCROLLINFO lpsi);
BOOL    WINAPI ShowScrollBar(HWND hWnd, int wBar, BOOL bShow);
BOOL    WINAPI EnableScrollBar(HWND hWnd, UINT wSBflags, UINT wArrows);
DWORD   WINAPI GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId);
BOOL    WINAPI AttachThreadInput(DWORD idAttach, DWORD idAttachTo, BOOL fAttach);
HWND    WINAPI SetParent(HWND hWndChild, HWND hWndNewParent);
HWND    WINAPI GetParent(HWND hWnd);
HWND    WINAPI GetAncestor(HWND hWnd, UINT gaFlags);
HWND    WINAPI GetTopWindow(HWND hWnd);
BOOL    WINAPI BringWindowToTop(HWND hWnd);
HWND    WINAPI GetWindow(HWND hWnd, UINT uCmd);
BOOL    WINAPI IsChild(HWND hWndParent, HWND hWnd);
BOOL    WINAPI EnumChildWindows(HWND hWndParent, WNDENUMPROC lpEnumFunc, LPARAM lParam);
int     WINAPI GetDlgCtrlID(HWND hWnd);
HWND    WINAPI ChildWindowFromPoint(HWND hWndParent, POINT Point);
HWND    WINAPI ChildWindowFromPointEx(HWND hWndParent, POINT Point, UINT uFlags);
HWND    WINAPI RealChildWindowFromPoint(HWND hwndParent, POINT ptParentClientCoords);
HWND    WINAPI WindowFromPoint(POINT Point);
HWND    WINAPI FindWindowA(LPCSTR lpClassName, LPCSTR lpWindowName);
HWND    WINAPI FindWindowExA(HWND hWndParent, HWND hWndChildAfter, LPCSTR lpClassName, LPCSTR lpWindowName);
BOOL    WINAPI EnumWindows(WNDENUMPROC lpEnumFunc, LPARAM lParam);
HWND    WINAPI SetCapture(HWND hWnd);
BOOL    WINAPI ReleaseCapture(void);
HWND    WINAPI GetCapture(void);
SHORT   WINAPI GetKeyState(int nVirtKey);
SHORT   WINAPI GetAsyncKeyState(int vKey);
BOOL    WINAPI PostThreadMessageA(DWORD idThread, UINT Msg, WPARAM wParam, LPARAM lParam);
void    WINAPI PostQuitMessage(int nExitCode);
BOOL    WINAPI WaitMessage(void);
DWORD   WINAPI MsgWaitForMultipleObjects(DWORD nCount, const HANDLE* pHandles, BOOL bWaitAll, DWORD dwMilliseconds, DWORD dwWakeMask);
DWORD   WINAPI MsgWaitForMultipleObjectsEx(DWORD nCount, const HANDLE* pHandles, DWORD dwMilliseconds, DWORD dwWakeMask, DWORD dwFlags);
DWORD   WINAPI GetQueueStatus(UINT flags);
BOOL    WINAPI IsHungAppWindow(HWND hWnd);

BOOL    WINAPI OpenClipboard(HWND hWndNewOwner);
BOOL    WINAPI CloseClipboard(void);
BOOL    WINAPI EmptyClipboard(void);
BOOL    WINAPI IsClipboardFormatAvailable(UINT format);
HGLOBAL WINAPI SetClipboardData(UINT uFormat, HGLOBAL hMem);
HGLOBAL WINAPI GetClipboardData(UINT uFormat);

HMENU   WINAPI CreateMenu(void);
HMENU   WINAPI CreatePopupMenu(void);
BOOL    WINAPI AppendMenuA(HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem);
BOOL    WINAPI InsertMenuA(HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem);
BOOL    WINAPI ModifyMenuA(HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem);
BOOL    WINAPI RemoveMenu(HMENU hMenu, UINT uPosition, UINT uFlags);
BOOL    WINAPI DeleteMenu(HMENU hMenu, UINT uPosition, UINT uFlags);
UINT    WINAPI CheckMenuItem(HMENU hMenu, UINT uIDCheckItem, UINT uCheck);
UINT    WINAPI EnableMenuItem(HMENU hMenu, UINT uIDEnableItem, UINT uEnable);
BOOL    WINAPI SetMenu(HWND hWnd, HMENU hMenu);
HMENU   WINAPI GetMenu(HWND hWnd);
BOOL    WINAPI DrawMenuBar(HWND hWnd);
BOOL    WINAPI DestroyMenu(HMENU hMenu);
HMENU   WINAPI GetSubMenu(HMENU hMenu, int nPos);
int     WINAPI GetMenuItemCount(HMENU hMenu);
UINT    WINAPI GetMenuItemID(HMENU hMenu, int nPos);
BOOL    WINAPI GetMenuItemInfoA(HMENU hMenu, UINT item, BOOL fByPosition, LPMENUITEMINFOA lpmii);
HMENU   WINAPI GetSystemMenu(HWND hWnd, BOOL bRevert);
BOOL    WINAPI TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd, const void* prcRect);
BOOL    WINAPI TrackPopupMenuEx(HMENU hMenu, UINT uFlags, int x, int y, HWND hWnd, const TPMPARAMS* lptpm);
HACCEL  WINAPI CreateAcceleratorTableA(LPACCEL lpaccl, int cEntries);
BOOL    WINAPI DestroyAcceleratorTable(HACCEL hAccel);
int     WINAPI TranslateAcceleratorA(HWND hWnd, HACCEL hAccTable, LPMSG lpMsg);

#ifdef __cplusplus
}
#endif

#ifndef UNICODE
#define WNDCLASSEX WNDCLASSEXA
#define CREATESTRUCT CREATESTRUCTA
#define MDICREATESTRUCT MDICREATESTRUCTA
#define LPMDICREATESTRUCT LPMDICREATESTRUCTA
#define LPCREATESTRUCT LPCREATESTRUCTA
#define RegisterClassEx RegisterClassExA
#define UnregisterClass UnregisterClassA
#define CreateWindowEx  CreateWindowExA
#define PostMessage     PostMessageA
#define SendMessage     SendMessageA
#define SendMessageTimeout SendMessageTimeoutA
#define PeekMessage     PeekMessageA
#define GetMessage      GetMessageA
#define DispatchMessage DispatchMessageA
#define DefWindowProc   DefWindowProcA
#define DefDlgProc      DefDlgProcA
#define DefFrameProc    DefFrameProcA
#define DefMDIChildProc DefMDIChildProcA
#define IsDialogMessage IsDialogMessageA
#define GetWindowLongPtr GetWindowLongPtrA
#define SetWindowLongPtr SetWindowLongPtrA
#define GetWindowLong    GetWindowLongA
#define SetWindowLong    SetWindowLongA
#define GetClassLongPtr  GetClassLongPtrA
#define SetClassLongPtr  SetClassLongPtrA
#define GetClassLong     GetClassLongA
#define SetClassLong     SetClassLongA
#define CallWindowProc   CallWindowProcA
#define GetClassName     GetClassNameA
#define GetWindowText    GetWindowTextA
#define SetWindowText    SetWindowTextA
#define GetWindowTextLength GetWindowTextLengthA
#define SetDlgItemText   SetDlgItemTextA
#define GetDlgItemText   GetDlgItemTextA
#define SendDlgItemMessage SendDlgItemMessageA
#define PostThreadMessage PostThreadMessageA
#define FindWindow       FindWindowA
#define FindWindowEx     FindWindowExA
#define AppendMenu       AppendMenuA
#define InsertMenu       InsertMenuA
#define ModifyMenu       ModifyMenuA
#define GetMenuItemInfo  GetMenuItemInfoA
#define CreateAcceleratorTable CreateAcceleratorTableA
#define TranslateAccelerator TranslateAcceleratorA
#endif
