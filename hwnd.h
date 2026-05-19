#pragma once
#include "mytypes.h"
#include "capability.h"
#include "ipc.h"
#include "myqueue.h"
#include "mywindow_state.h"
#include <pthread.h>
#include <stddef.h>

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

// ─────────────────────────────────────────────
//  HWND System
//  Jedes Fenster hat eine eindeutige ID.
//  Jede Interaktion ist eine Message.
// ─────────────────────────────────────────────

// ── Messages - WinAPI-Stil ───────────────────
#ifndef WM_CREATE
#define WM_CREATE       0x0001  // fenster erstellt
#endif
#ifndef WM_DESTROY
#define WM_DESTROY      0x0002  // fenster zerstört
#endif
#ifndef WM_MOVE
#define WM_MOVE         0x0003  // position geändert, lParam=MAKELPARAM(x,y)
#endif
#ifndef WM_NCCREATE
#define WM_NCCREATE     0x0081  // non-client create, lParam = CREATESTRUCT*
#endif
#ifndef WM_NCDESTROY
#define WM_NCDESTROY    0x0082  // non-client destroy, letztes lifecycle-signal
#endif
#ifndef WM_NCHITTEST
#define WM_NCHITTEST    0x0084  // non-client hit-test, lParam = screen coords
#endif
#ifndef WM_GETDLGCODE
#define WM_GETDLGCODE   0x0087  // dialog manager asks a control which keys it wants
#endif
#ifndef WM_NCMOUSEMOVE
#define WM_NCMOUSEMOVE  0x00A0  // non-client mouse move
#endif
#ifndef WM_NCLBUTTONDOWN
#define WM_NCLBUTTONDOWN 0x00A1 // non-client left button down, wParam=HT*
#endif
#ifndef WM_NCLBUTTONUP
#define WM_NCLBUTTONUP   0x00A2 // non-client left button up, wParam=HT*
#endif
#ifndef WM_NCRBUTTONDOWN
#define WM_NCRBUTTONDOWN 0x00A4  // non-client right button down, wParam=HT*, lParam=screen coords
#endif
#ifndef WM_NCRBUTTONUP
#define WM_NCRBUTTONUP   0x00A5  // non-client right button up
#endif
#ifndef WM_ACTIVATE
#define WM_ACTIVATE     0x0006  // aktivierung/fokus geändert
#endif
#ifndef WM_SETFOCUS
#define WM_SETFOCUS     0x0007  // focus gained
#endif
#ifndef WM_KILLFOCUS
#define WM_KILLFOCUS    0x0008  // focus lost
#endif
#ifndef WM_ENABLE
#define WM_ENABLE       0x000A  // enabled/disabled changed
#endif
#ifndef WM_SETTEXT
#define WM_SETTEXT      0x000C  // set window/control text
#endif
#ifndef WM_GETTEXT
#define WM_GETTEXT      0x000D  // get window/control text
#endif
#ifndef WM_GETTEXTLENGTH
#define WM_GETTEXTLENGTH 0x000E // get text length
#endif
#ifndef WM_SHOWWINDOW
#define WM_SHOWWINDOW   0x0018  // sichtbar/minimiert geändert
#endif
#ifndef WM_PAINT
#define WM_PAINT        0x000F  // neu zeichnen
#endif
#ifndef WM_CLOSE
#define WM_CLOSE        0x0010  // schließen angefordert
#endif
#ifndef WM_SIZE
#define WM_SIZE         0x0005  // größe geändert
#endif
#ifndef WM_GETMINMAXINFO
#define WM_GETMINMAXINFO 0x0024 // lParam=MINMAXINFO*
#endif
#ifndef WM_NEXTDLGCTL
#define WM_NEXTDLGCTL   0x0028  // dialog manager focus navigation
#endif
#ifndef WM_WINDOWPOSCHANGING
#define WM_WINDOWPOSCHANGING 0x0046 // lParam=WINDOWPOS*
#endif
#ifndef WM_WINDOWPOSCHANGED
#define WM_WINDOWPOSCHANGED 0x0047  // lParam=WINDOWPOS*
#endif
#ifndef WM_KEYDOWN
#define WM_KEYDOWN      0x0100  // taste gedrückt
#endif
#ifndef WM_KEYUP
#define WM_KEYUP        0x0101  // taste losgelassen
#endif
#ifndef WM_CHAR
#define WM_CHAR         0x0102  // zeichen eingabe
#endif
#ifndef WM_SYSKEYDOWN
#define WM_SYSKEYDOWN   0x0104  // system key down (Alt-combo)
#endif
#ifndef WM_SYSKEYUP
#define WM_SYSKEYUP     0x0105  // system key up
#endif
#ifndef WM_SYSCHAR
#define WM_SYSCHAR      0x0106  // system char (Alt-combo)
#endif
#ifndef WM_INITDIALOG
#define WM_INITDIALOG  0x0110  // dialog init, wParam=focus, lParam=init param
#endif
#ifndef WM_COMMAND
#define WM_COMMAND      0x0111  // menu/accelerator/control command
#endif
#ifndef WM_SYSCOMMAND
#define WM_SYSCOMMAND   0x0112  // system command (SC_CLOSE/SC_MOVE/...)
#endif
#ifndef WM_INITMENU
#define WM_INITMENU     0x0116  // menu is about to become active
#endif
#ifndef WM_INITMENUPOPUP
#define WM_INITMENUPOPUP 0x0117 // popup menu is about to be shown
#endif
#ifndef WM_MENUSELECT
#define WM_MENUSELECT   0x011F  // menu selection changed
#endif
#ifndef WM_ENTERMENULOOP
#define WM_ENTERMENULOOP 0x0211 // modal menu loop entered
#endif
#ifndef WM_EXITMENULOOP
#define WM_EXITMENULOOP  0x0212 // modal menu loop exited
#endif
#ifndef WM_CANCELMODE
#define WM_CANCELMODE    0x001F // cancel modal state/menu/capture
#endif
#ifndef WM_MOUSEMOVE
#define WM_MOUSEMOVE    0x0200  // maus bewegt
#endif
#ifndef WM_LBUTTONDOWN
#define WM_LBUTTONDOWN  0x0201  // links gedrückt
#endif
#ifndef WM_LBUTTONUP
#define WM_LBUTTONUP    0x0202  // links losgelassen
#endif
#ifndef WM_RBUTTONDOWN
#define WM_RBUTTONDOWN  0x0204  // rechts gedrückt
#endif
#ifndef WM_RBUTTONUP
#define WM_RBUTTONUP    0x0205  // rechts losgelassen
#endif
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL   0x020A  // scrollrad
#endif
#ifndef WM_CAPTURECHANGED
#define WM_CAPTURECHANGED 0x0215  // capture owner changed
#endif
#ifndef WM_ENTERSIZEMOVE
#define WM_ENTERSIZEMOVE 0x0231  // enters modal move/size loop
#endif
#ifndef WM_EXITSIZEMOVE
#define WM_EXITSIZEMOVE  0x0232  // exits modal move/size loop
#endif
#ifndef WM_TIMER
#define WM_TIMER        0x0113  // timer event
#endif
#ifndef WM_SURFACE_DIRTY
#define WM_SURFACE_DIRTY 0x0310  // surface/paint-state dirty
#endif
#ifndef WM_USER
#define WM_USER         0x0400  // app-eigene messages ab hier
#endif
#define DM_GETDEFID     (WM_USER + 0)
#define DM_SETDEFID     (WM_USER + 1)

#define WM_WINDOWTEXTCHANGED (WM_USER + 0x0050)  // myOS: Titeltext geändert

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


#define SIZE_RESTORED  0u
#define SIZE_MINIMIZED 1u
#define SIZE_MAXIMIZED 2u

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

#define WM_MYOS_WINDOWSTATE   (WM_USER + 0x0100)  // lParam/payload: MyWindowState* in-process PoC
#define WM_MYOS_SUBSCRIBED    (WM_USER + 0x0101)
#define WM_MYOS_HWND_STATE_DIRTY          (WM_USER + 0x0102)  // v74: shared HWND state dirty, wParam=source HWND, lParam=serial
#define WM_MYOS_HWND_STATE_SUBSCRIBE_REQ  (WM_USER + 0x0103)  // v74: OOP child asks parent to subscribe this HWND globally
#define WM_MYOS_HWND_STATE_UNSUBSCRIBE_REQ (WM_USER + 0x0104) // v74: OOP child asks parent to unsubscribe

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

#define BN_CLICKED 0
#define BN_PAINT 1
#define BN_HILITE 2
#define BN_UNHILITE 3
#define BN_DISABLE 4
#define BN_DOUBLECLICKED 5
#define BN_SETFOCUS 6
#define BN_KILLFOCUS 7
#define IDOK      1
#define IDCANCEL  2
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
#ifndef EN_CHANGE
#define EN_CHANGE 0x0300u
#endif
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


/* v94 STATIC control styles (MSDN-compatible subset) */
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
#define SS_CENTERIMAGE      0x00000200u
#define SS_NOTIFY           0x00000100u

/* v94 EDIT control styles/messages (MSDN-compatible subset) */
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
#ifndef EN_CHANGE
#define EN_CHANGE           0x0300u
#endif
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

/* v92 common controls / scroll messages (MSDN-compatible subset) */
#ifndef WM_HSCROLL
#define WM_HSCROLL      0x0114
#endif
#ifndef WM_VSCROLL
#define WM_VSCROLL      0x0115
#endif

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
#define LB_SELECTSTRING   0x018Cu
#define LB_DIR            0x018Du
#define LB_GETTOPINDEX    0x018Eu
#define LB_FINDSTRING     0x018Fu
#define LB_GETSELCOUNT    0x0190u
#define LB_GETSELITEMS    0x0191u
#define LB_SETTABSTOPS    0x0192u
#define LB_GETHORIZONTALEXTENT 0x0193u
#define LB_SETHORIZONTALEXTENT 0x0194u
#define LB_SETCOLUMNWIDTH 0x0195u
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

#define LBN_ERRSPACE     (-2)
#define LBN_SELCHANGE     1
#define LBN_DBLCLK        2
#define LBN_SELCANCEL     3
#define LBN_SETFOCUS      4
#define LBN_KILLFOCUS     5

#ifndef LBS_NOTIFY
#define LBS_NOTIFY        0x0001L
#endif
#ifndef LBS_SORT
#define LBS_SORT          0x0002L
#endif
#ifndef LBS_NOREDRAW
#define LBS_NOREDRAW      0x0004L
#endif
#ifndef LBS_MULTIPLESEL
#define LBS_MULTIPLESEL   0x0008L
#endif
#ifndef LBS_OWNERDRAWFIXED
#define LBS_OWNERDRAWFIXED 0x0010L
#endif
#ifndef LBS_HASSTRINGS
#define LBS_HASSTRINGS    0x0040L
#endif
#ifndef LBS_USETABSTOPS
#define LBS_USETABSTOPS   0x0080L
#endif
#ifndef LBS_NOINTEGRALHEIGHT
#define LBS_NOINTEGRALHEIGHT 0x0100L
#endif
#ifndef LBS_MULTICOLUMN
#define LBS_MULTICOLUMN   0x0200L
#endif
#ifndef LBS_WANTKEYBOARDINPUT
#define LBS_WANTKEYBOARDINPUT 0x0400L
#endif
#ifndef LBS_EXTENDEDSEL
#define LBS_EXTENDEDSEL   0x0800L
#endif

#define SB_HORZ           0
#define SB_VERT           1
#define SB_CTL            2
#define SB_BOTH           3
#define SB_LINEUP         0
#define SB_LINELEFT       0
#define SB_LINEDOWN       1
#define SB_LINERIGHT      1
#define SB_PAGEUP         2
#define SB_PAGELEFT       2
#define SB_PAGEDOWN       3
#define SB_PAGERIGHT      3
#define SB_THUMBPOSITION  4
#define SB_THUMBTRACK     5
#define SB_TOP            6
#define SB_LEFT           6
#define SB_BOTTOM         7
#define SB_RIGHT          7
#define SB_ENDSCROLL      8

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

// ── Hook-Rückgabewerte ────────────────────────

// ── Non-client hit test codes (MSDN-compatible subset) ─────────────
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

// ── WM_SYSCOMMAND values (high nibble significant, like Win32) ─────
#define SC_SIZE      0xF000u
#define SC_MOVE      0xF010u
#define SC_MINIMIZE  0xF020u
#define SC_MAXIMIZE  0xF030u
#define SC_CLOSE     0xF060u
#define SC_KEYMENU   0xF100u
#define SC_RESTORE   0xF120u

#define HOOK_ALLOW    0   // message durchlassen
#define HOOK_BLOCK    1   // message blockieren
#define HOOK_MODIFIED 2   // message verändert, weiter

// ── WndProc - was jedes Fenster registriert ───
typedef void (*HWNDWndProc)(HWND hwnd, UINT msg,
                            WPARAM wparam, LPARAM lparam,
                            void* userdata);

// ── Hook - kann Messages abfangen/verändern ───
typedef int (*HookProc)(HWND target, UINT msg,
                        WPARAM* wparam, LPARAM* lparam,
                        void* userdata);

#define MAX_HOOKS      8
#define MAX_HWNDS      64
#define MAX_UI_THREADS 32
#define MAX_SUBSCRIPTIONS 64
#define MAX_USER_TIMERS 32
#define HWND_THREAD_HASH_BUCKETS 64
#define HWND_THREAD_HASH_MASK (HWND_THREAD_HASH_BUCKETS - 1)

/* v219: HWNDs are USER handles, but internally they now follow the same
   slot/generation/state discipline as the Object Manager.  The public value
   remains opaque; internally it decodes to a stable window slot plus a
   generation so stale HWND numbers cannot silently resolve after reuse. */
#define _HWND_TAG        0x71000000u
#define _HWND_TAG_MASK   0xff000000u
#define _HWND_GEN_SHIFT  16u
#define _HWND_GEN_MASK   0x00ff0000u
#define _HWND_SLOT_MASK  0x0000ffffu

typedef enum _HwndState {
    _HWND_STATE_FREE = 0,
    _HWND_STATE_RESERVED = 1,
    _HWND_STATE_NCCREATE = 2,
    _HWND_STATE_LIVE = 3,
    _HWND_STATE_DESTROY_PENDING = 4,
    _HWND_STATE_NCDESTROY = 5,
    _HWND_STATE_ZOMBIE = 6
} _HwndState;

/* v221: USER32 actions are private dispatch classes derived from the
   resolved HWND state.  Public WinAPI names stay public; these are internal
   state-machine edges. */
typedef enum _HwndAction {
    _HWND_ACTION_QUERY    = 0x00000001u,
    _HWND_ACTION_MESSAGE  = 0x00000002u,
    _HWND_ACTION_MUTATE   = 0x00000004u,
    _HWND_ACTION_SHOW     = 0x00000008u,
    _HWND_ACTION_FOCUS    = 0x00000010u,
    _HWND_ACTION_CAPTURE  = 0x00000020u,
    _HWND_ACTION_PAINT    = 0x00000040u,
    _HWND_ACTION_DESTROY  = 0x00000080u,
    _HWND_ACTION_GEOMETRY = 0x00000100u,
    _HWND_ACTION_HITTEST  = 0x00000200u
} _HwndAction;

typedef struct _HwndHeader {
    DWORD cbSize;
    HWND  hwnd;
    DWORD hwnd_slot;
    DWORD hwnd_generation;
    DWORD hwnd_state;
    DWORD owner_pid;
    DWORD owner_tid;
    DWORD state_flags;
    DWORD update_serial;
} _HwndHeader;

typedef struct {
    HookProc   proc;
    void*      userdata;
    Capability cap;        // wer hat den hook gesetzt
    uint32_t   hook_id;    // zum entfernen
} Hook;

typedef struct MyUserTimer {
    int        valid;
    HWND       hwnd;
    UINT_PTR   id;
    UINT       elapse_ms;
    uint64_t   next_due_ns;
    uintptr_t  callback;
    uint64_t   fire_count;
} MyUserTimer;

typedef struct {
    uint32_t       tid;
    uint32_t       pid;
    int            valid;
    int            hash_next;      // v240: bucket chain for O(1)-average PID/TID queue lookup
    DWORD          owner_window_count; // v240: avoids scanning HWND table when tearing queues down
    int            external_pump;  // v20: Queue wird von App/UI-Thread selbst gepumpt
    int            in_dispatch;    // v20: WndProc laeuft gerade
    uint64_t       dispatch_start_ns;
    uint64_t       last_pump_ns;
    uint64_t       dispatch_count;
    int            in_send_dispatch;   // v21: sync SendMessage dispatch active
    uint32_t       send_depth;         // v21: reentrancy/deadlock guard hint
    uint64_t       send_count;
    uint64_t       send_timeout_count;

    /* v149 USER32 timers live on the UI-thread queue, not in the KERNEL32
       waitable-timer dispatcher.  GetMessage/PeekMessage synthesize WM_TIMER
       only after queued input/window messages are exhausted. */
    MyUserTimer    timers[MAX_USER_TIMERS];
    UINT_PTR       next_timer_id;

    char           name[32];
    MyMessageQueue queue;
} MyThreadQueue;

// ── HWND Eintrag im OS ────────────────────────
typedef struct MYOS_CACHELINE_ALIGN HWNDEntry {
    /* v241: keep the resolve/dispatch header in the first cache line.  The
       old v240 layout placed the large Capability before generation/state and
       owner IDs, so hwnd_query_header() had to step over cold token data on
       every hot HWND lookup. */
    HWND       hwnd;        // opaque USER handle value
    uint32_t   hwnd_slot;
    uint32_t   hwnd_generation;
    uint32_t   hwnd_state;
    uint32_t   owner_pid;
    uint32_t   owner_tid;
    int        valid;       // 1 = exists/resolves
    uint32_t   hot_flags;   // v241: hot mirror of MyWindowState.flags
    uint32_t   hot_update_serial; // v241: hot mirror of MyWindowState.updateSerial
    HWNDWndProc wndproc;    // message handler
    void*      userdata;
    unsigned char _hot_pad[MYOS_CACHELINE_BYTES - 56u];

    // Cold/token metadata.
    Capability cap;         // token des fensters
} HWNDEntry;

_Static_assert(offsetof(HWNDEntry, cap) >= MYOS_CACHELINE_BYTES,
               "HWNDEntry cold Capability must not share the hot resolve cacheline");
_Static_assert(sizeof(HWNDEntry) % MYOS_CACHELINE_BYTES == 0,
               "HWNDEntry array stride must preserve 64-byte alignment");

// ── Hook Chain ────────────────────────────────
typedef struct {
    Hook     hooks[MAX_HOOKS];
    int      count;
    uint32_t next_id;
} HookChain;

typedef struct HWNDSubscription {
    int    valid;
    HWND   source;
    HWND   subscriber;
    UINT   msgMin;
    UINT   msgMax;
    uint32_t subscriber_pid;
} HWNDSubscription;

// ── HWND Manager ─────────────────────────────
typedef struct {
    HWNDEntry      entries[MAX_HWNDS];
    MyThreadQueue  threads[MAX_UI_THREADS];
    int            count;
    HWND           next_hwnd;      /* legacy monotonic counter retained for diagnostics */
    uint32_t       hwnd_generations[MAX_HWNDS];
    int            hwnd_free_stack[MAX_HWNDS];     // v240: O(1)-average HWND slot reuse
    int            hwnd_free_top;
    int            thread_free_stack[MAX_UI_THREADS]; // v240: O(1)-average UI-thread queue allocation
    int            thread_free_top;
    int            thread_hash[HWND_THREAD_HASH_BUCKETS]; // v240: PID/TID -> queue index
    HookChain      chain;
    HWNDSubscription subscriptions[MAX_SUBSCRIPTIONS];
    uint32_t       subscription_count;

    // v17/v74: shared/read-mostly window state section. Queue carries only signal;
    // readers copy current state from this section. v74 mirrors this into
    // a real named FileMapping section for OOP readers.
    MyWindowStateSection state_section;
    MyWindowStateSection* state_section_mirror;

    pthread_mutex_t lock;   // schützt entries, thread registry und hook-chain
} HWNDManager;


// v19: lightweight process/app snapshot derived from HWND ownership.
typedef struct MyProcessInfo {
    DWORD cbSize;
    DWORD pid;
    CHAR  szName[32];
    DWORD capabilityFlags;
    DWORD hwndCount;
    DWORD uiQueueDepth;
    BOOL  alive;
} MyProcessInfo;

typedef BOOL (*MYPROCESSENUMPROC)(const MyProcessInfo* lpInfo, LPARAM lParam);

// Aggregierte Queue-Statistik für Debug-HUD.
typedef struct HWNDManagerStats {
    uint64_t posted;
    uint64_t dispatched;
    uint64_t dropped;
    uint64_t coalesced;
    uint64_t peak_depth;
    uint32_t current_depth;
    uint32_t thread_count;
    uint32_t hwnd_count;
    uint32_t subscription_count;
    uint32_t state_version;
    uint32_t state_capacity;
    uint32_t state_active;
    uint32_t state_destroyed;
    uint32_t hung_windows;
    uint64_t send_count;
    uint64_t send_timeouts;
} HWNDManagerStats;

// Lifecycle
void hwnd_manager_init(HWNDManager* mgr);
void hwnd_manager_destroy(HWNDManager* mgr);

// CreateWindow equivalent
HWND hwnd_create(HWNDManager* mgr, HWNDWndProc proc,
                 void* userdata, Capability cap);

// v41: CreateWindowExA muss WM_NCCREATE/WM_CREATE selbst in Windows-Reihenfolge
// auslösen, nachdem Parent/ID/Text/Style/lpParam gespeichert sind. Legacy
// hwnd_create() bleibt kompatibel und sendet weiterhin WM_CREATE direkt.
HWND hwnd_create_no_create(HWNDManager* mgr, HWNDWndProc proc,
                           void* userdata, Capability cap);

// DestroyWindow
void hwnd_destroy(HWNDManager* mgr, HWND hwnd);

// PostMessage - async
int hwnd_post(HWNDManager* mgr, const Capability* sender,
              HWND target, UINT msg, WPARAM wp, LPARAM lp);
int hwnd_post_routed(HWNDManager* mgr, const Capability* sender,
                     HWND target, UINT msg, WPARAM wp, LPARAM lp,
                     const _MsgRouteDescriptor* route);

// SendMessage - sync
int hwnd_send(HWNDManager* mgr, const Capability* sender,
              HWND target, UINT msg, WPARAM wp, LPARAM lp);

// Hook setzen - nur mit CAP_HOOK
uint32_t hwnd_set_hook(HWNDManager* mgr, HookProc proc,
                       void* userdata, Capability cap);

// Hook entfernen
void hwnd_remove_hook(HWNDManager* mgr, uint32_t hook_id);

// Dispatch - alle UI-Thread-Queues leeren
void hwnd_dispatch(HWNDManager* mgr);

// WinAPI-näher: Queue-Zugriff über HWND oder owner pid/tid.
int hwnd_get_message(HWNDManager* mgr, HWND hwnd, MyMessage* out);
int hwnd_get_message_wait(HWNDManager* mgr, HWND hwnd, MyMessage* out, int timeout_ms);
int hwnd_get_thread_message(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                            HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                            int remove, MyMessage* out);
int hwnd_get_thread_message_wait(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                                 HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                                 int remove, int timeout_ms, MyMessage* out);
int hwnd_remove_thread_messages(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                                HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax);
int hwnd_remove_queued_messages_for_hwnd(HWNDManager* mgr, HWND hwnd, UINT wMsgFilterMin, UINT wMsgFilterMax);

/* v149 USER32 SetTimer/KillTimer backing.  callback is a TIMERPROC pointer
   stored as an integer so hwnd.h does not depend on public winuser.h typedefs. */
UINT_PTR hwnd_set_user_timer(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                             HWND hwnd, UINT_PTR idEvent, UINT elapse_ms, uintptr_t callback);
int hwnd_kill_user_timer(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                         HWND hwnd, UINT_PTR idEvent);

int hwnd_dispatch_message(HWNDManager* mgr, const MyMessage* msg);

// SendMessageTimeout-Grundlage.
int hwnd_send_timeout(HWNDManager* mgr, const Capability* sender,
                      HWND target, UINT msg, WPARAM wp, LPARAM lp,
                      int timeout_ms);

// myOS subscription primitive: subscriber receives matching source-window messages async.
int hwnd_subscribe(HWNDManager* mgr, const Capability* subscriberCap,
                   HWND source, HWND subscriber, UINT msgMin, UINT msgMax);
int hwnd_unsubscribe(HWNDManager* mgr, const Capability* subscriberCap,
                     HWND source, HWND subscriber, UINT msgMin, UINT msgMax);
int hwnd_publish_from_window(HWNDManager* mgr, HWND source, UINT msg, WPARAM wp, LPARAM lp);
int hwnd_update_window_state(HWNDManager* mgr, const MyWindowState* state);
int hwnd_copy_window_state(HWNDManager* mgr, HWND hwnd, MyWindowState* out);
const MyWindowStateSection* hwnd_get_window_state_section(HWNDManager* mgr);
int hwnd_attach_window_state_section(HWNDManager* mgr, MyWindowStateSection* sharedView);
uint64_t hwnd_get_owner_visual_signature(HWNDManager* mgr, DWORD ownerPid, HWND rootHwnd);
int hwnd_decode(HWND hwnd, DWORD* outSlot, DWORD* outGeneration);
int hwnd_state_allows(DWORD state, DWORD action);
int hwnd_query_action(HWNDManager* mgr, HWND hwnd, DWORD action, _HwndHeader* out);
int hwnd_query_header(HWNDManager* mgr, HWND hwnd, _HwndHeader* out);
int hwnd_set_state(HWNDManager* mgr, HWND hwnd, DWORD state);
int hwnd_is_window(HWNDManager* mgr, HWND hwnd);
uint32_t hwnd_get_owner_pid(HWNDManager* mgr, HWND hwnd);
uint32_t hwnd_get_owner_tid(HWNDManager* mgr, HWND hwnd);
int hwnd_get_process_info(HWNDManager* mgr, DWORD pid, MyProcessInfo* out);
int hwnd_enum_processes(HWNDManager* mgr, MYPROCESSENUMPROC proc, LPARAM lParam);

// v20: UI-thread pump/hung helpers. external_pump means hwnd_dispatch() leaves this queue alone.
int  hwnd_set_thread_external_pump(HWNDManager* mgr, DWORD pid, DWORD tid, int enabled, const char* name);
int  hwnd_is_window_hung(HWNDManager* mgr, HWND hwnd, DWORD timeout_ms);
int  hwnd_post_thread_message(HWNDManager* mgr, const Capability* sender, DWORD tid, UINT msg, WPARAM wp, LPARAM lp);
DWORD hwnd_get_thread_queue_status(HWNDManager* mgr, DWORD pid, DWORD tid);
DWORD hwnd_get_thread_queue_status_bits(HWNDManager* mgr, DWORD pid, DWORD tid, UINT flags);
int hwnd_thread_queue_has_status(HWNDManager* mgr, DWORD pid, DWORD tid, UINT flags);
uint64_t hwnd_get_thread_send_count(HWNDManager* mgr, DWORD pid, DWORD tid);
uint64_t hwnd_get_thread_send_timeout_count(HWNDManager* mgr, DWORD pid, DWORD tid);

void hwnd_get_stats(HWNDManager* mgr, HWNDManagerStats* out);
