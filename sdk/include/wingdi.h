#pragma once
/* myOS Win32 SDK - wingdi.h */
#include "windef.h"
#ifdef __cplusplus
extern "C" {
#endif

#ifndef LF_FACESIZE
#define LF_FACESIZE 32
#endif

#define FW_DONTCARE     0
#define FW_THIN         100
#define FW_EXTRALIGHT   200
#define FW_LIGHT        300
#define FW_NORMAL       400
#define FW_REGULAR      400
#define FW_MEDIUM       500
#define FW_SEMIBOLD     600
#define FW_BOLD         700
#define FW_EXTRABOLD    800
#define FW_HEAVY        900

#define ANSI_CHARSET        0u
#define DEFAULT_CHARSET     1u
#define SYMBOL_CHARSET      2u
#define SHIFTJIS_CHARSET    128u
#define HANGEUL_CHARSET     129u
#define HANGUL_CHARSET      129u
#define GB2312_CHARSET      134u
#define CHINESEBIG5_CHARSET 136u
#define OEM_CHARSET         255u

#define OUT_DEFAULT_PRECIS  0u
#define CLIP_DEFAULT_PRECIS 0u
#define DEFAULT_QUALITY     0u
#define DEFAULT_PITCH       0u
#define FIXED_PITCH         1u
#define VARIABLE_PITCH      2u
#define FF_DONTCARE         0x00u
#define FF_ROMAN            0x10u
#define FF_SWISS            0x20u
#define FF_MODERN           0x30u
#define FF_SCRIPT           0x40u
#define FF_DECORATIVE       0x50u

typedef struct tagLOGFONTA {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    CHAR lfFaceName[LF_FACESIZE];
} LOGFONTA, *PLOGFONTA, *NPLOGFONTA, *LPLOGFONTA;

#ifndef SRCCOPY
#define SRCCOPY 0x00CC0020u
#endif

#ifndef PATCOPY
#define PATCOPY   0x00F00021u
#define PATINVERT 0x005A0049u
#define DSTINVERT 0x00550009u
#define BLACKNESS 0x00000042u
#define WHITENESS 0x00FF0062u
#endif
#ifndef CLR_INVALID
#define CLR_INVALID 0xFFFFFFFFu
#endif
#ifndef GDI_ERROR
#define GDI_ERROR 0xFFFFFFFFu
#endif

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
#ifndef RGN_AND
#define RGN_AND  1
#define RGN_OR   2
#define RGN_XOR  3
#define RGN_DIFF 4
#define RGN_COPY 5
#endif



#ifndef BLACKONWHITE
#define BLACKONWHITE 1
#define WHITEONBLACK 2
#define COLORONCOLOR 3
#define HALFTONE 4
#define STRETCH_ANDSCANS BLACKONWHITE
#define STRETCH_ORSCANS WHITEONBLACK
#define STRETCH_DELETESCANS COLORONCOLOR
#define STRETCH_HALFTONE HALFTONE
#endif

#ifndef BI_RGB
#define BI_RGB 0u
#endif
#ifndef BI_BITFIELDS
#define BI_BITFIELDS 3u
#endif
#ifndef DIB_RGB_COLORS
#define DIB_RGB_COLORS 0u
#endif
#ifndef DIB_PAL_COLORS
#define DIB_PAL_COLORS 1u
#endif

typedef struct tagRGBQUAD {
    BYTE rgbBlue;
    BYTE rgbGreen;
    BYTE rgbRed;
    BYTE rgbReserved;
} RGBQUAD, *LPRGBQUAD;

typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD          bmiColors[1];
} BITMAPINFO, *PBITMAPINFO, *LPBITMAPINFO;

typedef struct tagBITMAP {
    LONG   bmType;
    LONG   bmWidth;
    LONG   bmHeight;
    LONG   bmWidthBytes;
    WORD   bmPlanes;
    WORD   bmBitsPixel;
    LPVOID bmBits;
} BITMAP, *PBITMAP, *NPBITMAP, *LPBITMAP;

typedef struct tagDIBSECTION {
    BITMAP           dsBm;
    BITMAPINFOHEADER dsBmih;
    DWORD            dsBitfields[3];
    HANDLE           dshSection;
    DWORD            dsOffset;
} DIBSECTION, *PDIBSECTION, *LPDIBSECTION;

typedef struct tagPAINTSTRUCT {
    HDC  hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT, *PPAINTSTRUCT, *LPPAINTSTRUCT;

BOOL    WINAPI InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase);
BOOL    WINAPI ValidateRect(HWND hWnd, const RECT* lpRect);
BOOL    WINAPI GetUpdateRect(HWND hWnd, LPRECT lpRect, BOOL bErase);
BOOL    WINAPI UpdateWindow(HWND hWnd);
BOOL    WINAPI RedrawWindow(HWND hWnd, const RECT* lprcUpdate, HRGN hrgnUpdate, UINT flags);
HDC     WINAPI BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint);
BOOL    WINAPI EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint);
HDC     WINAPI GetDC(HWND hWnd);
int     WINAPI ReleaseDC(HWND hWnd, HDC hDC);
HDC     WINAPI CreateCompatibleDC(HDC hdc);
BOOL    WINAPI DeleteDC(HDC hdc);
HBRUSH  WINAPI CreateSolidBrush(COLORREF color);
HBITMAP WINAPI CreateCompatibleBitmap(HDC hdc, int cx, int cy);
HBITMAP WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes, UINT nBitCount, const void* lpBits);
HBITMAP WINAPI CreateDIBSection(HDC hdc, const BITMAPINFO* pbmi, UINT iUsage, void** ppvBits, HANDLE hSection, DWORD dwOffset);
BOOL    WINAPI DeleteObject(HGDIOBJ hObject);
HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ hObject);
int     WINAPI FillRect(HDC hDC, const RECT* lprc, HBRUSH hbr);
BOOL    WINAPI Rectangle(HDC hDC, int left, int top, int right, int bottom);
BOOL    WINAPI TextOutA(HDC hDC, int x, int y, LPCSTR lpString, int c);
int     WINAPI DrawTextA(HDC hDC, LPCSTR lpchText, int cchText, LPRECT lprc, UINT format);
int     WINAPI GetObjectA(HGDIOBJ hgdiobj, int cbBuffer, LPVOID lpvObject);
COLORREF WINAPI SetPixel(HDC hdc, int x, int y, COLORREF color);
COLORREF WINAPI GetPixel(HDC hdc, int x, int y);
BOOL    WINAPI PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop);
BOOL    WINAPI BitBlt(HDC hdcDest, int xDest, int yDest, int w, int h, HDC hdcSrc, int xSrc, int ySrc, DWORD rop);
BOOL    WINAPI StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop);
int     WINAPI GetStretchBltMode(HDC hdc);
int     WINAPI SetStretchBltMode(HDC hdc, int mode);
int     WINAPI GetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT cLines, void* lpvBits, BITMAPINFO* lpbmi, UINT usage);
int     WINAPI SetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT cLines, const void* lpBits, const BITMAPINFO* lpbmi, UINT ColorUse);
int     WINAPI StretchDIBits(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight, int xSrc, int ySrc, int SrcWidth, int SrcHeight, const void* lpBits, const BITMAPINFO* lpbmi, UINT iUsage, DWORD rop);
int     WINAPI SetDIBitsToDevice(HDC hdc, int xDest, int yDest, DWORD dwWidth, DWORD dwHeight, int xSrc, int ySrc, UINT uStartScan, UINT cScanLines, const void* lpvBits, const BITMAPINFO* lpbmi, UINT fuColorUse);

HRGN    WINAPI CreateRectRgn(int x1, int y1, int x2, int y2);
HRGN    WINAPI CreateRectRgnIndirect(const RECT* lprc);
BOOL    WINAPI SetRectRgn(HRGN hrgn, int left, int top, int right, int bottom);
int     WINAPI OffsetRgn(HRGN hrgn, int x, int y);
int     WINAPI GetRgnBox(HRGN hrgn, LPRECT lprc);
int     WINAPI CombineRgn(HRGN hrgnDst, HRGN hrgnSrc1, HRGN hrgnSrc2, int iMode);
BOOL    WINAPI EqualRgn(HRGN hrgn1, HRGN hrgn2);
BOOL    WINAPI PtInRegion(HRGN hrgn, int x, int y);
BOOL    WINAPI RectInRegion(HRGN hrgn, const RECT* lprc);
BOOL    WINAPI InvalidateRgn(HWND hWnd, HRGN hRgn, BOOL bErase);
BOOL    WINAPI ValidateRgn(HWND hWnd, HRGN hRgn);
int     WINAPI GetUpdateRgn(HWND hWnd, HRGN hRgn, BOOL bErase);
int     WINAPI SelectClipRgn(HDC hdc, HRGN hrgn);
int     WINAPI ExcludeClipRect(HDC hdc, int left, int top, int right, int bottom);
int     WINAPI IntersectClipRect(HDC hdc, int left, int top, int right, int bottom);
int     WINAPI GetClipBox(HDC hdc, LPRECT lprc);

#ifdef __cplusplus
}
#endif
#ifndef UNICODE
#define LOGFONT LOGFONTA
#define TextOut TextOutA
#define DrawText DrawTextA
#define GetObject GetObjectA
#endif
