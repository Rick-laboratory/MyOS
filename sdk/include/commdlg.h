#pragma once
/* myOS Win32 SDK - commdlg.h */
#include "windef.h"
#include "winuser.h"
#include "wingdi.h"
#ifdef __cplusplus
extern "C" {
#endif

#define CDERR_DIALOGFAILURE   0xFFFFu
#define CDERR_GENERALCODES    0x0000u
#define CDERR_STRUCTSIZE      0x0001u
#define CDERR_INITIALIZATION  0x0002u
#define CDERR_NOTEMPLATE      0x0003u
#define CDERR_NOHINSTANCE     0x0004u
#define CDERR_LOADSTRFAILURE  0x0005u
#define CDERR_FINDRESFAILURE  0x0006u
#define CDERR_LOADRESFAILURE  0x0007u
#define CDERR_LOCKRESFAILURE  0x0008u
#define CDERR_MEMALLOCFAILURE 0x0009u
#define CDERR_MEMLOCKFAILURE  0x000Au
#define CDERR_NOHOOK          0x000Bu
#define CDERR_REGISTERMSGFAIL 0x000Cu
#define FNERR_FILENAMECODES   0x3000u
#define FNERR_SUBCLASSFAILURE 0x3001u
#define FNERR_INVALIDFILENAME 0x3002u
#define FNERR_BUFFERTOOSMALL  0x3003u

#define OFN_READONLY                 0x00000001u
#define OFN_OVERWRITEPROMPT          0x00000002u
#define OFN_HIDEREADONLY             0x00000004u
#define OFN_NOCHANGEDIR              0x00000008u
#define OFN_SHOWHELP                 0x00000010u
#define OFN_ENABLEHOOK               0x00000020u
#define OFN_ENABLETEMPLATE           0x00000040u
#define OFN_ENABLETEMPLATEHANDLE     0x00000080u
#define OFN_NOVALIDATE               0x00000100u
#define OFN_ALLOWMULTISELECT         0x00000200u
#define OFN_EXTENSIONDIFFERENT       0x00000400u
#define OFN_PATHMUSTEXIST            0x00000800u
#define OFN_FILEMUSTEXIST            0x00001000u
#define OFN_CREATEPROMPT             0x00002000u
#define OFN_SHAREAWARE               0x00004000u
#define OFN_NOREADONLYRETURN         0x00008000u
#define OFN_NOTESTFILECREATE         0x00010000u
#define OFN_NONETWORKBUTTON          0x00020000u
#define OFN_NOLONGNAMES              0x00040000u
#define OFN_EXPLORER                 0x00080000u
#define OFN_NODEREFERENCELINKS       0x00100000u
#define OFN_LONGNAMES                0x00200000u
#define OFN_ENABLEINCLUDENOTIFY      0x00400000u
#define OFN_ENABLESIZING             0x00800000u
#define OFN_DONTADDTORECENT          0x02000000u
#define OFN_FORCESHOWHIDDEN          0x10000000u


#ifndef WM_NOTIFY
#define WM_NOTIFY 0x004Eu
#endif

#ifndef NMHDR_DEFINED
#define NMHDR_DEFINED
typedef struct tagNMHDR {
    HWND     hwndFrom;
    UINT_PTR idFrom;
    UINT     code;
} NMHDR, *LPNMHDR;
#endif

#ifndef CDN_FIRST
#define CDN_FIRST          ((UINT)-601)
#define CDN_INITDONE       (CDN_FIRST - 0x0000u)
#define CDN_SELCHANGE      (CDN_FIRST - 0x0001u)
#define CDN_FOLDERCHANGE   (CDN_FIRST - 0x0002u)
#define CDN_SHAREVIOLATION (CDN_FIRST - 0x0003u)
#define CDN_HELP           (CDN_FIRST - 0x0004u)
#define CDN_FILEOK         (CDN_FIRST - 0x0005u)
#define CDN_TYPECHANGE     (CDN_FIRST - 0x0006u)
#define CDN_INCLUDEITEM    (CDN_FIRST - 0x0007u)
#endif

#ifndef CDM_FIRST
#define CDM_FIRST          (WM_USER + 100u)
#define CDM_GETSPEC        (CDM_FIRST + 0u)
#define CDM_GETFILEPATH    (CDM_FIRST + 1u)
#define CDM_GETFOLDERPATH  (CDM_FIRST + 2u)
#define CDM_GETFOLDERIDLIST (CDM_FIRST + 3u)
#define CDM_SETCONTROLTEXT (CDM_FIRST + 4u)
#define CDM_HIDECONTROL    (CDM_FIRST + 5u)
#define CDM_SETDEFEXT      (CDM_FIRST + 6u)
#endif

struct tagOFNA;
typedef UINT_PTR (CALLBACK *LPOFNHOOKPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct tagOFNA {
    DWORD        lStructSize;
    HWND         hwndOwner;
    HINSTANCE    hInstance;
    LPCSTR       lpstrFilter;
    LPSTR        lpstrCustomFilter;
    DWORD        nMaxCustFilter;
    DWORD        nFilterIndex;
    LPSTR        lpstrFile;
    DWORD        nMaxFile;
    LPSTR        lpstrFileTitle;
    DWORD        nMaxFileTitle;
    LPCSTR       lpstrInitialDir;
    LPCSTR       lpstrTitle;
    DWORD        Flags;
    WORD         nFileOffset;
    WORD         nFileExtension;
    LPCSTR       lpstrDefExt;
    LPARAM       lCustData;
    LPOFNHOOKPROC lpfnHook;
    LPCSTR       lpTemplateName;
    LPVOID       pvReserved;
    DWORD        dwReserved;
    DWORD        FlagsEx;
} OPENFILENAMEA, *LPOPENFILENAMEA;

typedef struct tagOFNOTIFYA {
    NMHDR           hdr;
    LPOPENFILENAMEA lpOFN;
    LPSTR           pszFile;
} OFNOTIFYA, *LPOFNOTIFYA;

typedef struct tagOFNOTIFYEXA {
    NMHDR           hdr;
    LPOPENFILENAMEA lpOFN;
    LPVOID          psf;
    LPVOID          pidl;
} OFNOTIFYEXA, *LPOFNOTIFYEXA;

#define REGULAR_FONTTYPE 0x0400u
#define BOLD_FONTTYPE    0x0100u
#define ITALIC_FONTTYPE  0x0200u
#define SCREEN_FONTTYPE  0x2000u
#define PRINTER_FONTTYPE 0x4000u
#define SIMULATED_FONTTYPE 0x8000u

#define CF_SCREENFONTS           0x00000001u
#define CF_PRINTERFONTS          0x00000002u
#define CF_BOTH                  (CF_SCREENFONTS | CF_PRINTERFONTS)
#define CF_SHOWHELP              0x00000004u
#define CF_ENABLEHOOK            0x00000008u
#define CF_ENABLETEMPLATE        0x00000010u
#define CF_ENABLETEMPLATEHANDLE  0x00000020u
#define CF_INITTOLOGFONTSTRUCT   0x00000040u
#define CF_USESTYLE              0x00000080u
#define CF_EFFECTS               0x00000100u
#define CF_APPLY                 0x00000200u
#define CF_ANSIONLY              0x00000400u
#define CF_SCRIPTSONLY           CF_ANSIONLY
#define CF_NOVECTORFONTS         0x00000800u
#define CF_NOOEMFONTS            CF_NOVECTORFONTS
#define CF_NOSIMULATIONS         0x00001000u
#define CF_LIMITSIZE             0x00002000u
#define CF_FIXEDPITCHONLY        0x00004000u
#define CF_WYSIWYG               0x00008000u
#define CF_FORCEFONTEXIST        0x00010000u
#define CF_SCALABLEONLY          0x00020000u
#define CF_TTONLY                0x00040000u
#define CF_NOFACESEL             0x00080000u
#define CF_NOSTYLESEL            0x00100000u
#define CF_NOSIZESEL             0x00200000u
#define CF_SELECTSCRIPT          0x00400000u
#define CF_NOSCRIPTSEL           0x00800000u
#define CF_NOVERTFONTS           0x01000000u

#define CFERR_CHOOSEFONTCODES 0x2000u
#define CFERR_NOFONTS         0x2001u
#define CFERR_MAXLESSTHANMIN  0x2002u

typedef UINT_PTR (CALLBACK *LPCFHOOKPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct tagCHOOSEFONTA {
    DWORD        lStructSize;
    HWND         hwndOwner;
    HDC          hDC;
    LPLOGFONTA   lpLogFont;
    int          iPointSize;
    DWORD        Flags;
    COLORREF     rgbColors;
    LPARAM       lCustData;
    LPCFHOOKPROC lpfnHook;
    LPCSTR       lpTemplateName;
    HINSTANCE    hInstance;
    LPSTR        lpszStyle;
    WORD         nFontType;
    WORD         ___MISSING_ALIGNMENT__;
    int          nSizeMin;
    int          nSizeMax;
} CHOOSEFONTA, *LPCHOOSEFONTA;

BOOL  WINAPI GetOpenFileNameA(LPOPENFILENAMEA lpofn);
BOOL  WINAPI GetSaveFileNameA(LPOPENFILENAMEA lpofn);
DWORD WINAPI CommDlgExtendedError(void);
BOOL  WINAPI ChooseFontA(LPCHOOSEFONTA lpcf);

#ifdef __cplusplus
}
#endif
#ifndef UNICODE
#define OPENFILENAME OPENFILENAMEA
#define GetOpenFileName GetOpenFileNameA
#define GetSaveFileName GetSaveFileNameA
#define CHOOSEFONT CHOOSEFONTA
#define ChooseFont ChooseFontA
#endif
