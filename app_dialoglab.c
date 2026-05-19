#include "app_dialoglab.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "window.h"
#include "mycontrols.h"
#include "app_msdn_resize.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>

/* AUDIT(v119-lab): DialogLab is the highest-value regression detector. It
   depends on modal-loop parent disabling, DLGTEMPLATE parsing, IsDialogMessage,
   tab order, default buttons, radio-arrow navigation, menu dispatch and COMDLG.
   If this breaks after a refactor, do not assume the template is wrong first:
   inspect queue ownership, focus HWND, DM_SETDEFID/DM_GETDEFID, and whether
   WM_COMMAND lParam still carries the child HWND. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define DLAB_CMD_MODAL     0x8801u
#define DLAB_CMD_MODELESS  0x8802u
#define DLAB_CMD_DUMP      0x8803u
#define DLAB_CMD_CONTROLS  0x8804u
#define DLAB_CMD_BUTTONS   0x8805u
#define DLAB_CMD_TEXT      0x8806u
#define DLAB_CMD_KEYBOARD  0x8807u
#define DLAB_CMD_SCROLLSTD 0x8808u
#define DLAB_CMD_MENU      0x8809u
#define DLAB_CMD_DUMPNAV   0x880Au
#define DLAB_CMD_OPENFILE  0x880Bu
#define DLAB_CMD_SAVEFILE  0x880Cu
#define DLAB_CMD_CHOOSEFONT 0x880Du
#define DLAB_MENU_FILE_OPEN 0x9701u
#define DLAB_MENU_FILE_EXIT 0x9702u
#define DLAB_MENU_VIEW_CHECK 0x9703u
#define DLAB_MENU_VIEW_DISABLED 0x9704u
#define DLAB_MENU_OWNERDRAW 0x9705u
#define DLAB_MENU_RECENT_ONE 0x9708u
#define DLAB_MENU_RECENT_TWO 0x9709u
#define DLAB_EDIT_NAME     101
#define DLAB_LIST_ITEMS    201
#define DLAB_COMBO_ITEMS   203
#define DLAB_SCROLL_VALUE  205
#define DLAB_CHECK_AUTO    301
#define DLAB_CHECK_3STATE  302
#define DLAB_RADIO_ONE     303
#define DLAB_RADIO_TWO     304
#define DLAB_RADIO_THREE   305
#define DLAB_PUSH_NORMAL   306
#define DLAB_PUSH_DEFAULT  307
#define DLAB_EDIT_NORMAL   401
#define DLAB_EDIT_PASS     402
#define DLAB_EDIT_READONLY 403
#define DLAB_EDIT_MULTI    404
#define DLAB_STATIC_LEFT   405
#define DLAB_STATIC_CENTER 406
#define DLAB_STATIC_RIGHT  407
#define DLAB_KEY_NAME      501
#define DLAB_KEY_PASS      502
#define DLAB_KEY_APPLY     503
#define DLAB_KEY_R1        504
#define DLAB_KEY_R2        505
#define DLAB_KEY_R3        506


typedef struct MyDialogLabAccessProbeTemplate {
    DLGTEMPLATE dlg;
    WORD menu;
    WORD windowClass;
    WORD title[13];
    WORD pointSize;
    WORD typeface[13];

    DLGITEMTEMPLATE itemStatic;
    WORD staticClass[2];
    WORD staticTitle[6];
    WORD staticExtra;

    DLGITEMTEMPLATE itemEdit;
    WORD editClass[2];
    WORD editTitle[1];
    WORD editExtra;
    WORD editPad;

    DLGITEMTEMPLATE itemOk;
    WORD okClass[2];
    WORD okTitle[3];
    WORD okExtra;
    WORD okPad;

    DLGITEMTEMPLATE itemCancel;
    WORD cancelClass[2];
    WORD cancelTitle[7];
    WORD cancelExtra;
} MyDialogLabAccessProbeTemplate;

static const MyDialogLabAccessProbeTemplate g_AccessProbeDialogTemplate __attribute__((aligned(4))) = {
    .dlg = {
        .style = WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE,
        .dwExtendedStyle = 0,
        .cdit = 4,
        .x = 0, .y = 0, .cx = 213, .cy = 63
    },
    .menu = 0,
    .windowClass = 0,
    .title = { 'A','c','c','e','s','s',' ','P','r','o','b','e',0 },
    .pointSize = 8,
    .typeface = { 'M','S',' ','S','h','e','l','l',' ','D','l','g',0 },

    .itemStatic = { .style = WS_CHILD | WS_VISIBLE, .dwExtendedStyle = 0, .x = 12, .y = 17, .cx = 41, .cy = 14, .id = 100 },
    .staticClass = { 0xFFFFu, MYOS_DLG_CLASS_STATIC },
    .staticTitle = { 'N','a','m','e',':',0 },
    .staticExtra = 0,

    .itemEdit = { .style = WS_CHILD | WS_VISIBLE | WS_TABSTOP, .dwExtendedStyle = 0, .x = 55, .y = 15, .cx = 140, .cy = 17, .id = DLAB_EDIT_NAME },
    .editClass = { 0xFFFFu, MYOS_DLG_CLASS_EDIT },
    .editTitle = { 0 },
    .editExtra = 0,
    .editPad = 0,

    .itemOk = { .style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, .dwExtendedStyle = 0, .x = 64, .y = 39, .cx = 57, .cy = 17, .id = IDOK },
    .okClass = { 0xFFFFu, MYOS_DLG_CLASS_BUTTON },
    .okTitle = { 'O','K',0 },
    .okExtra = 0,
    .okPad = 0,

    .itemCancel = { .style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, .dwExtendedStyle = 0, .x = 127, .y = 39, .cx = 57, .cy = 17, .id = IDCANCEL },
    .cancelClass = { 0xFFFFu, MYOS_DLG_CLASS_BUTTON },
    .cancelTitle = { 'C','a','n','c','e','l',0 },
    .cancelExtra = 0
};


static BYTE g_ControlProbeTemplateBlob[1536] __attribute__((aligned(4)));
static int  g_ControlProbeTemplateReady = 0;
static BYTE g_ButtonProbeTemplateBlob[1536] __attribute__((aligned(4)));
static int  g_ButtonProbeTemplateReady = 0;
static BYTE g_TextProbeTemplateBlob[2048] __attribute__((aligned(4)));
static int  g_TextProbeTemplateReady = 0;
static BYTE g_KeyboardProbeTemplateBlob[2048] __attribute__((aligned(4)));
static int  g_KeyboardProbeTemplateReady = 0;

static void dlab_blob_align4(BYTE** pp)
{
    uintptr_t v = (uintptr_t)(*pp);
    v = (v + 3u) & ~(uintptr_t)3u;
    *pp = (BYTE*)v;
}

static void dlab_blob_w16(BYTE** pp, WORD v)
{
    memcpy(*pp, &v, sizeof(v));
    *pp += sizeof(v);
}

static void dlab_blob_w32(BYTE** pp, DWORD v)
{
    memcpy(*pp, &v, sizeof(v));
    *pp += sizeof(v);
}

static void dlab_blob_wstr(BYTE** pp, const char* s)
{
    if (!s) s = "";
    while (*s) dlab_blob_w16(pp, (WORD)(unsigned char)*s++);
    dlab_blob_w16(pp, 0);
}

static void dlab_blob_ord(BYTE** pp, WORD atom)
{
    dlab_blob_w16(pp, 0xFFFFu);
    dlab_blob_w16(pp, atom);
}

static void dlab_blob_item(BYTE** pp, DWORD style, DWORD exStyle, short x, short y, short cx, short cy, WORD id, WORD clsAtom, const char* title)
{
    dlab_blob_align4(pp);
    dlab_blob_w32(pp, style);
    dlab_blob_w32(pp, exStyle);
    dlab_blob_w16(pp, (WORD)x);
    dlab_blob_w16(pp, (WORD)y);
    dlab_blob_w16(pp, (WORD)cx);
    dlab_blob_w16(pp, (WORD)cy);
    dlab_blob_w16(pp, id);
    dlab_blob_ord(pp, clsAtom);
    dlab_blob_wstr(pp, title);
    dlab_blob_w16(pp, 0); /* creation data bytes */
}

static LPCDLGTEMPLATEA dlab_build_control_probe_template(void)
{
    if (g_ControlProbeTemplateReady) return (LPCDLGTEMPLATEA)g_ControlProbeTemplateBlob;
    memset(g_ControlProbeTemplateBlob, 0, sizeof(g_ControlProbeTemplateBlob));
    BYTE* p = g_ControlProbeTemplateBlob;

    dlab_blob_w32(&p, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    dlab_blob_w32(&p, 0);
    dlab_blob_w16(&p, 8);      /* cdit */
    dlab_blob_w16(&p, 0); dlab_blob_w16(&p, 0); dlab_blob_w16(&p, 280); dlab_blob_w16(&p, 135);
    dlab_blob_w16(&p, 0);      /* no menu */
    dlab_blob_w16(&p, 0);      /* default #32770 */
    dlab_blob_wstr(&p, "Common Controls Probe");
    dlab_blob_w16(&p, 8);      /* DS_SETFONT point size */
    dlab_blob_wstr(&p, "MS Shell Dlg");

    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE, 0, 12, 12, 42, 10, 200, MYOS_DLG_CLASS_STATIC, "List:");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY, 0, 12, 25, 92, 70, DLAB_LIST_ITEMS, MYOS_DLG_CLASS_LISTBOX, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE, 0, 116, 12, 54, 10, 202, MYOS_DLG_CLASS_STATIC, "Combo:");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 0, 116, 25, 92, 17, DLAB_COMBO_ITEMS, MYOS_DLG_CLASS_COMBOBOX, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE, 0, 220, 12, 54, 10, 204, MYOS_DLG_CLASS_STATIC, "Scroll:");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | SBS_VERT, 0, 228, 25, 12, 70, DLAB_SCROLL_VALUE, MYOS_DLG_CLASS_SCROLLBAR, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 84, 108, 57, 17, IDOK, MYOS_DLG_CLASS_BUTTON, "&OK");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 148, 108, 57, 17, IDCANCEL, MYOS_DLG_CLASS_BUTTON, "&Cancel");

    g_ControlProbeTemplateReady = 1;
    return (LPCDLGTEMPLATEA)g_ControlProbeTemplateBlob;
}


static LPCDLGTEMPLATEA dlab_build_button_probe_template(void)
{
    if (g_ButtonProbeTemplateReady) return (LPCDLGTEMPLATEA)g_ButtonProbeTemplateBlob;
    memset(g_ButtonProbeTemplateBlob, 0, sizeof(g_ButtonProbeTemplateBlob));
    BYTE* p = g_ButtonProbeTemplateBlob;

    dlab_blob_w32(&p, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    dlab_blob_w32(&p, 0);
    dlab_blob_w16(&p, 10);      /* cdit */
    dlab_blob_w16(&p, 0); dlab_blob_w16(&p, 0); dlab_blob_w16(&p, 300); dlab_blob_w16(&p, 132);
    dlab_blob_w16(&p, 0);      /* no menu */
    dlab_blob_w16(&p, 0);      /* default #32770 */
    dlab_blob_wstr(&p, "Button Family Probe");
    dlab_blob_w16(&p, 8);
    dlab_blob_wstr(&p, "MS Shell Dlg");

    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 10, 10, 128, 76, 300, MYOS_DLG_CLASS_BUTTON, "GroupBox / &Checks");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 22, 28, 96, 12, DLAB_CHECK_AUTO, MYOS_DLG_CLASS_BUTTON, "&AutoCheckBox");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTO3STATE, 0, 22, 46, 96, 12, DLAB_CHECK_3STATE, MYOS_DLG_CLASS_BUTTON, "Auto&3State");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 150, 10, 132, 76, 310, MYOS_DLG_CLASS_BUTTON, "&Radio Group");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 0, 162, 28, 88, 12, DLAB_RADIO_ONE, MYOS_DLG_CLASS_BUTTON, "&Radio One");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 162, 46, 88, 12, DLAB_RADIO_TWO, MYOS_DLG_CLASS_BUTTON, "Radio &Two");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 162, 64, 88, 12, DLAB_RADIO_THREE, MYOS_DLG_CLASS_BUTTON, "Radio T&hree");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON, 0, 28, 101, 68, 17, DLAB_PUSH_NORMAL, MYOS_DLG_CLASS_BUTTON, "&Push");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 112, 101, 68, 17, IDOK, MYOS_DLG_CLASS_BUTTON, "&OK");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 196, 101, 68, 17, IDCANCEL, MYOS_DLG_CLASS_BUTTON, "&Cancel");

    g_ButtonProbeTemplateReady = 1;
    return (LPCDLGTEMPLATEA)g_ButtonProbeTemplateBlob;
}


static LPCDLGTEMPLATEA dlab_build_text_probe_template(void)
{
    if (g_TextProbeTemplateReady) return (LPCDLGTEMPLATEA)g_TextProbeTemplateBlob;
    memset(g_TextProbeTemplateBlob, 0, sizeof(g_TextProbeTemplateBlob));
    BYTE* p = g_TextProbeTemplateBlob;

    dlab_blob_w32(&p, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    dlab_blob_w32(&p, 0);
    dlab_blob_w16(&p, 15);
    dlab_blob_w16(&p, 0); dlab_blob_w16(&p, 0); dlab_blob_w16(&p, 342); dlab_blob_w16(&p, 172);
    dlab_blob_w16(&p, 0);
    dlab_blob_w16(&p, 0);
    dlab_blob_wstr(&p, "Static/Edit Probe");
    dlab_blob_w16(&p, 8);
    dlab_blob_wstr(&p, "MS Shell Dlg");

    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 12, 12, 80, 10, DLAB_STATIC_LEFT, MYOS_DLG_CLASS_STATIC, "SS_LEFT");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 104, 12, 80, 10, DLAB_STATIC_CENTER, MYOS_DLG_CLASS_STATIC, "SS_CENTER");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_RIGHT, 0, 196, 12, 80, 10, DLAB_STATIC_RIGHT, MYOS_DLG_CLASS_STATIC, "SS_RIGHT");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0, 12, 29, 304, 3, 408, MYOS_DLG_CLASS_STATIC, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE, 0, 12, 38, 56, 10, 409, MYOS_DLG_CLASS_STATIC, "Normal:");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 70, 35, 170, 17, DLAB_EDIT_NORMAL, MYOS_DLG_CLASS_EDIT, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE, 0, 12, 61, 56, 10, 410, MYOS_DLG_CLASS_STATIC, "Password:");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 0, 70, 58, 170, 17, DLAB_EDIT_PASS, MYOS_DLG_CLASS_EDIT, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE, 0, 12, 84, 56, 10, 411, MYOS_DLG_CLASS_STATIC, "Readonly:");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL, 0, 70, 81, 170, 17, DLAB_EDIT_READONLY, MYOS_DLG_CLASS_EDIT, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE, 0, 12, 107, 56, 10, 412, MYOS_DLG_CLASS_STATIC, "Multiline:");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 70, 104, 210, 42, DLAB_EDIT_MULTI, MYOS_DLG_CLASS_EDIT, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, 0, 286, 35, 36, 111, 413, MYOS_DLG_CLASS_STATIC, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 106, 151, 57, 17, IDOK, MYOS_DLG_CLASS_BUTTON, "&OK");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 170, 151, 57, 17, IDCANCEL, MYOS_DLG_CLASS_BUTTON, "&Cancel");

    g_TextProbeTemplateReady = 1;
    return (LPCDLGTEMPLATEA)g_TextProbeTemplateBlob;
}

static LPCDLGTEMPLATEA dlab_build_keyboard_probe_template(void)
{
    if (g_KeyboardProbeTemplateReady) return (LPCDLGTEMPLATEA)g_KeyboardProbeTemplateBlob;
    memset(g_KeyboardProbeTemplateBlob, 0, sizeof(g_KeyboardProbeTemplateBlob));
    BYTE* p = g_KeyboardProbeTemplateBlob;

    dlab_blob_w32(&p, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    dlab_blob_w32(&p, 0);
    dlab_blob_w16(&p, 12);
    dlab_blob_w16(&p, 0); dlab_blob_w16(&p, 0); dlab_blob_w16(&p, 310); dlab_blob_w16(&p, 136);
    dlab_blob_w16(&p, 0);
    dlab_blob_w16(&p, 0);
    dlab_blob_wstr(&p, "Keyboard/Nav Probe");
    dlab_blob_w16(&p, 8);
    dlab_blob_wstr(&p, "MS Shell Dlg");

    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE, 0, 12, 16, 60, 10, 500, MYOS_DLG_CLASS_STATIC, "&Name:");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 76, 13, 130, 17, DLAB_KEY_NAME, MYOS_DLG_CLASS_EDIT, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE, 0, 12, 40, 60, 10, 501, MYOS_DLG_CLASS_STATIC, "&Password:");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 0, 76, 37, 130, 17, DLAB_KEY_PASS, MYOS_DLG_CLASS_EDIT, "");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 218, 10, 78, 62, 502, MYOS_DLG_CLASS_BUTTON, "&Mode");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 0, 228, 26, 58, 12, DLAB_KEY_R1, MYOS_DLG_CLASS_BUTTON, "&One");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 228, 42, 58, 12, DLAB_KEY_R2, MYOS_DLG_CLASS_BUTTON, "&Two");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 228, 58, 58, 12, DLAB_KEY_R3, MYOS_DLG_CLASS_BUTTON, "T&hree");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON, 0, 44, 96, 64, 17, DLAB_KEY_APPLY, MYOS_DLG_CLASS_BUTTON, "&Apply");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 124, 96, 57, 17, IDOK, MYOS_DLG_CLASS_BUTTON, "&OK");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 188, 96, 57, 17, IDCANCEL, MYOS_DLG_CLASS_BUTTON, "&Cancel");
    dlab_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0, 12, 82, 284, 3, 507, MYOS_DLG_CLASS_STATIC, "");

    g_KeyboardProbeTemplateReady = 1;
    return (LPCDLGTEMPLATEA)g_KeyboardProbeTemplateBlob;
}

static void dlab_register_templates(void)
{
    RegisterDialogTemplateA("AccessProbeDialog", (LPCDLGTEMPLATEA)&g_AccessProbeDialogTemplate.dlg);
    RegisterDialogTemplateA("ControlProbeDialog", dlab_build_control_probe_template());
    RegisterDialogTemplateA("ButtonProbeDialog", dlab_build_button_probe_template());
    RegisterDialogTemplateA("TextProbeDialog", dlab_build_text_probe_template());
    RegisterDialogTemplateA("KeyboardProbeDialog", dlab_build_keyboard_probe_template());
}


typedef struct DialogLabApp {
    HWND hWnd;
    HWND hModal;
    HWND hModeless;
    HWND hControls;
    HWND hButtons;
    HWND hText;
    HWND hKeyboard;
    HMENU hMenuBar;
    HMENU hMenuFile;
    HMENU hMenuRecent;
    HMENU hMenuView;
    HMENU hMenuOwnerDraw;
    HWNDManager* mgr;
    Capability cap;
    MyAppResizeState resize;
    int modalRunning;
    int signalCount;
    INT_PTR lastResult;
    char lastResultText[32];
    char lastName[96];
    char status[220];
    char dump[4][180];
    pthread_t modalThread;
} DialogLabApp;

static DialogLabApp g_dlab;

static void dlab_dump_template(void)
{
    const MyDialogLabAccessProbeTemplate* t = &g_AccessProbeDialogTemplate;
    snprintf(g_dlab.dump[0], sizeof(g_dlab.dump[0]),
             "DLGTEMPLATE: style=0x%08x cdit=%u pos=(%d,%d) size=%dx%d DLU font=%upt MS Shell Dlg",
             (unsigned)t->dlg.style, (unsigned)t->dlg.cdit,
             (int)t->dlg.x, (int)t->dlg.y, (int)t->dlg.cx, (int)t->dlg.cy,
             (unsigned)t->pointSize);
    snprintf(g_dlab.dump[1], sizeof(g_dlab.dump[1]),
             "item0 STATIC atom=0x%04x id=%u dlu=(%d,%d %dx%d); item1 EDIT atom=0x%04x id=%u dlu=(%d,%d %dx%d)",
             MYOS_DLG_CLASS_STATIC, (unsigned)t->itemStatic.id,
             (int)t->itemStatic.x, (int)t->itemStatic.y, (int)t->itemStatic.cx, (int)t->itemStatic.cy,
             MYOS_DLG_CLASS_EDIT, (unsigned)t->itemEdit.id,
             (int)t->itemEdit.x, (int)t->itemEdit.y, (int)t->itemEdit.cx, (int)t->itemEdit.cy);
    snprintf(g_dlab.dump[2], sizeof(g_dlab.dump[2]),
             "item2 BUTTON atom=0x%04x id=IDOK dlu=(%d,%d %dx%d); item3 BUTTON id=IDCANCEL dlu=(%d,%d %dx%d)",
             MYOS_DLG_CLASS_BUTTON,
             (int)t->itemOk.x, (int)t->itemOk.y, (int)t->itemOk.cx, (int)t->itemOk.cy,
             (int)t->itemCancel.x, (int)t->itemCancel.y, (int)t->itemCancel.cx, (int)t->itemCancel.cy);
    snprintf(g_dlab.dump[3], sizeof(g_dlab.dump[3]),
             "Binary path: name->DLGTEMPLATE* supports BUTTON/EDIT/STATIC/LISTBOX/SCROLLBAR/COMBOBOX atoms");
    snprintf(g_dlab.status, sizeof(g_dlab.status), "Dump Template: parsed AccessProbeDialog resource blob");
}

typedef struct DlabNavDumpCtx {
    HWND hDlg;
    int count;
} DlabNavDumpCtx;

static BOOL CALLBACK dlab_nav_enum_proc(HWND hWnd, LPARAM lParam)
{
    DlabNavDumpCtx* ctx = (DlabNavDumpCtx*)lParam;
    if (!ctx) return FALSE;

    char cls[32] = {0};
    char text[56] = {0};
    GetClassNameA(hWnd, cls, sizeof(cls));
    GetWindowTextA(hWnd, text, sizeof(text));
    LONG style = GetWindowLongA(hWnd, GWL_STYLE);
    int id = GetDlgCtrlID(hWnd);
    LRESULT dlgCode = SendMessageA(hWnd, WM_GETDLGCODE, 0, 0);
    const char* focus = (GetFocus() == hWnd) ? " FOCUS" : "";

    if (ctx->count < 4) {
        snprintf(g_dlab.dump[ctx->count], sizeof(g_dlab.dump[ctx->count]),
                 "nav[%d] hwnd=%u id=%d class=%s style=0x%08lx dlgcode=0x%04lx%s text='%.24s'",
                 ctx->count, (unsigned)hWnd, id, cls[0] ? cls : "?",
                 (unsigned long)style, (unsigned long)dlgCode, focus, text);
    }
    ctx->count++;
    return TRUE;
}

static HWND dlab_active_probe_dialog(void)
{
    if (g_dlab.hKeyboard && IsWindow(g_dlab.hKeyboard)) return g_dlab.hKeyboard;
    if (g_dlab.hButtons && IsWindow(g_dlab.hButtons)) return g_dlab.hButtons;
    if (g_dlab.hControls && IsWindow(g_dlab.hControls)) return g_dlab.hControls;
    if (g_dlab.hText && IsWindow(g_dlab.hText)) return g_dlab.hText;
    if (g_dlab.hModeless && IsWindow(g_dlab.hModeless)) return g_dlab.hModeless;
    if (g_dlab.hModal && IsWindow(g_dlab.hModal)) return g_dlab.hModal;
    return 0;
}

static void dlab_dump_nav(void)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    for (int i = 0; i < 4; ++i) g_dlab.dump[i][0] = 0;

    HWND hDlg = dlab_active_probe_dialog();
    if (!hDlg) {
        snprintf(g_dlab.status, sizeof(g_dlab.status), "Dump Dialog Nav: open Keyboard/Button/Controls/Text dialog first");
        return;
    }

    DlabNavDumpCtx ctx;
    ctx.hDlg = hDlg;
    ctx.count = 0;
    EnumChildWindows(hDlg, dlab_nav_enum_proc, (LPARAM)&ctx);

    LRESULT def = SendMessageA(hDlg, DM_GETDEFID, 0, 0);
    snprintf(g_dlab.status, sizeof(g_dlab.status),
             "Dump Dialog Nav hDlg=%u children=%d focus=%u DM_GETDEFID=0x%lx",
             (unsigned)hDlg, ctx.count, (unsigned)GetFocus(), (unsigned long)def);
}

static void dlab_result(INT_PTR r, const char* name)
{
    char tmp[96];
    tmp[0] = 0;
    if (name) snprintf(tmp, sizeof(tmp), "%s", name);
    g_dlab.lastResult = r;
    snprintf(g_dlab.lastResultText, sizeof(g_dlab.lastResultText), "%s", r == IDOK ? "IDOK" : (r == IDCANCEL ? "IDCANCEL" : "OTHER"));
    if (name) snprintf(g_dlab.lastName, sizeof(g_dlab.lastName), "%s", tmp);
}

static void dlab_button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 24, COLOR(45,55,78));
    fb_rect_outline(fb, x, y, w, 24, COLOR(118,140,180));
    font_draw_str(fb, x + 8, y + 8, label, WHITE);
}

static void dlab_post_command(UINT cmd)
{
    if (!g_dlab.mgr || !g_dlab.hWnd) return;
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    hwnd_post(g_dlab.mgr, &g_dlab.cap, g_dlab.hWnd, WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
}

static void dlab_click(int x, int y)
{
    if (y >= 12 && y < 36) {
        if (x >= 14 && x < 174) { dlab_post_command(DLAB_CMD_MODAL); return; }
        if (x >= 184 && x < 360) { dlab_post_command(DLAB_CMD_MODELESS); return; }
        if (x >= 370 && x < 500) { dlab_post_command(DLAB_CMD_DUMP); return; }
        if (x >= 510 && x < 680) { dlab_post_command(DLAB_CMD_CONTROLS); return; }
        if (x >= 690 && x < 850) { dlab_post_command(DLAB_CMD_BUTTONS); return; }
    }
    if (y >= 42 && y < 66) {
        if (x >= 14 && x < 174) { dlab_post_command(DLAB_CMD_TEXT); return; }
        if (x >= 184 && x < 360) { dlab_post_command(DLAB_CMD_KEYBOARD); return; }
        if (x >= 370 && x < 550) { dlab_post_command(DLAB_CMD_SCROLLSTD); return; }
        if (x >= 560 && x < 730) { dlab_post_command(DLAB_CMD_MENU); return; }
        if (x >= 740 && x < 900) { dlab_post_command(DLAB_CMD_DUMPNAV); return; }
    }
    if (y >= 72 && y < 96) {
        if (x >= 14 && x < 214) { dlab_post_command(DLAB_CMD_OPENFILE); return; }
        if (x >= 224 && x < 424) { dlab_post_command(DLAB_CMD_SAVEFILE); return; }
        if (x >= 434 && x < 594) { dlab_post_command(DLAB_CMD_CHOOSEFONT); return; }
    }
}

static INT_PTR CALLBACK dlab_dialog_proc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (uMsg) {
    case WM_INITDIALOG:
        if (g_dlab.modalRunning) g_dlab.hModal = hDlg;
        if (GetDlgItem(hDlg, DLAB_EDIT_NAME)) {
            SetDlgItemTextA(hDlg, DLAB_EDIT_NAME, "test");
            snprintf(g_dlab.status, sizeof(g_dlab.status), "WM_INITDIALOG hDlg=%u -> SetDlgItemTextA(IDC_EDIT_NAME, 'test')", (unsigned)hDlg);
        }
        if (GetDlgItem(hDlg, DLAB_LIST_ITEMS)) {
            static const char* items[] = { "Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta" };
            for (int i = 0; i < 8; ++i) {
                SendDlgItemMessageA(hDlg, DLAB_LIST_ITEMS, LB_ADDSTRING, 0, (LPARAM)items[i]);
                SendDlgItemMessageA(hDlg, DLAB_COMBO_ITEMS, CB_ADDSTRING, 0, (LPARAM)items[i]);
            }
            SendDlgItemMessageA(hDlg, DLAB_LIST_ITEMS, LB_SETCURSEL, 1, 0);
            SendDlgItemMessageA(hDlg, DLAB_COMBO_ITEMS, CB_SETCURSEL, 2, 0);
            SendDlgItemMessageA(hDlg, DLAB_SCROLL_VALUE, SBM_SETRANGE, 0, 100);
            SendDlgItemMessageA(hDlg, DLAB_SCROLL_VALUE, SBM_SETPOS, 35, TRUE);
            g_dlab.hControls = hDlg;
            snprintf(g_dlab.status, sizeof(g_dlab.status), "ControlProbe WM_INITDIALOG: LB_ADDSTRING/CB_ADDSTRING/SBM_SETRANGE via SendDlgItemMessageA");
        }
        if (GetDlgItem(hDlg, DLAB_CHECK_AUTO)) {
            CheckDlgButton(hDlg, DLAB_CHECK_AUTO, BST_CHECKED);
            SendDlgItemMessageA(hDlg, DLAB_CHECK_3STATE, BM_SETCHECK, BST_INDETERMINATE, 0);
            CheckRadioButton(hDlg, DLAB_RADIO_ONE, DLAB_RADIO_THREE, DLAB_RADIO_ONE);
            g_dlab.hButtons = hDlg;
            snprintf(g_dlab.status, sizeof(g_dlab.status), "ButtonProbe WM_INITDIALOG: BS_GROUPBOX + auto checkbox/3state/radio initialized; Alt mnemonics active");
        }
        if (GetDlgItem(hDlg, DLAB_EDIT_NORMAL)) {
            SetDlgItemTextA(hDlg, DLAB_EDIT_NORMAL, "select me");
            SetDlgItemTextA(hDlg, DLAB_EDIT_PASS, "secret");
            SetDlgItemTextA(hDlg, DLAB_EDIT_READONLY, "read only text");
            SetDlgItemTextA(hDlg, DLAB_EDIT_MULTI, "line one\nline two\nline three");
            SendDlgItemMessageA(hDlg, DLAB_EDIT_NORMAL, EM_SETSEL, 0, 6);
            g_dlab.hText = hDlg;
            snprintf(g_dlab.status, sizeof(g_dlab.status), "TextProbe WM_INITDIALOG: STATIC styles + EDIT ES_PASSWORD/READONLY/MULTILINE/EM_SETSEL");
        }
        if (GetDlgItem(hDlg, DLAB_KEY_NAME)) {
            SetDlgItemTextA(hDlg, DLAB_KEY_NAME, "alt+n");
            SetDlgItemTextA(hDlg, DLAB_KEY_PASS, "secret");
            CheckRadioButton(hDlg, DLAB_KEY_R1, DLAB_KEY_R3, DLAB_KEY_R1);
            SendMessageA(hDlg, DM_SETDEFID, IDOK, 0);
            g_dlab.hKeyboard = hDlg;
            snprintf(g_dlab.status, sizeof(g_dlab.status), "KeyboardProbe WM_INITDIALOG: Alt mnemonics, WS_GROUP radio arrows, DM_SETDEFID");
        }
        return TRUE;
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        UINT code = HIWORD(wParam);
        if (id == DLAB_LIST_ITEMS && code == LBN_SELCHANGE) {
            int sel = (int)SendDlgItemMessageA(hDlg, DLAB_LIST_ITEMS, LB_GETCURSEL, 0, 0);
            char text[96] = {0};
            if (sel >= 0) SendDlgItemMessageA(hDlg, DLAB_LIST_ITEMS, LB_GETTEXT, (WPARAM)sel, (LPARAM)text);
            snprintf(g_dlab.status, sizeof(g_dlab.status), "LISTBOX LBN_SELCHANGE: sel=%d text='%s' count=%ld", sel, text, (long)SendDlgItemMessageA(hDlg, DLAB_LIST_ITEMS, LB_GETCOUNT, 0, 0));
            return TRUE;
        }
        if (id == DLAB_COMBO_ITEMS && (code == CBN_SELCHANGE || code == CBN_DROPDOWN || code == CBN_CLOSEUP || code == CBN_SELENDOK || code == CBN_SELENDCANCEL)) {
            int sel = (int)SendDlgItemMessageA(hDlg, DLAB_COMBO_ITEMS, CB_GETCURSEL, 0, 0);
            char text[96] = {0};
            if (sel >= 0) SendDlgItemMessageA(hDlg, DLAB_COMBO_ITEMS, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)text);
            snprintf(g_dlab.status, sizeof(g_dlab.status), "COMBOBOX notify=%u sel=%d text='%s' dropped=%ld", code, sel, text, (long)SendDlgItemMessageA(hDlg, DLAB_COMBO_ITEMS, CB_GETDROPPEDSTATE, 0, 0));
            return TRUE;
        }
        if ((id == DLAB_CHECK_AUTO || id == DLAB_CHECK_3STATE || id == DLAB_RADIO_ONE || id == DLAB_RADIO_TWO || id == DLAB_RADIO_THREE || id == DLAB_PUSH_NORMAL) && code == BN_CLICKED) {
            snprintf(g_dlab.status, sizeof(g_dlab.status),
                     "BUTTON BN_CLICKED id=%u checks: auto=%u tri=%u radio=[%u,%u,%u]",
                     id,
                     IsDlgButtonChecked(hDlg, DLAB_CHECK_AUTO),
                     IsDlgButtonChecked(hDlg, DLAB_CHECK_3STATE),
                     IsDlgButtonChecked(hDlg, DLAB_RADIO_ONE),
                     IsDlgButtonChecked(hDlg, DLAB_RADIO_TWO),
                     IsDlgButtonChecked(hDlg, DLAB_RADIO_THREE));
            return TRUE;
        }
        if ((id == DLAB_EDIT_NORMAL || id == DLAB_EDIT_PASS || id == DLAB_EDIT_READONLY || id == DLAB_EDIT_MULTI) && code == EN_CHANGE) {
            char text[128] = {0};
            GetDlgItemTextA(hDlg, (int)id, text, sizeof(text));
            snprintf(g_dlab.status, sizeof(g_dlab.status), "EDIT EN_CHANGE id=%u text='%s' sel=0x%08lx lines=%ld", id, text,
                     (long)SendDlgItemMessageA(hDlg, (int)id, EM_GETSEL, 0, 0),
                     (long)SendDlgItemMessageA(hDlg, (int)id, EM_GETLINECOUNT, 0, 0));
            return TRUE;
        }
        if (id == DLAB_KEY_APPLY && code == BN_CLICKED) {
            SendMessageA(hDlg, WM_NEXTDLGCTL, 0, FALSE);
            snprintf(g_dlab.status, sizeof(g_dlab.status), "WM_NEXTDLGCTL test: Apply moved focus to next tab item; DM_GETDEFID=0x%lx", (long)SendMessageA(hDlg, DM_GETDEFID, 0, 0));
            return TRUE;
        }
        if ((id == DLAB_KEY_R1 || id == DLAB_KEY_R2 || id == DLAB_KEY_R3) && code == BN_CLICKED) {
            snprintf(g_dlab.status, sizeof(g_dlab.status), "Keyboard radio group BN_CLICKED id=%u; arrow keys should move inside WS_GROUP", id);
            return TRUE;
        }
        if ((id == IDOK || id == IDCANCEL) && code != BN_CLICKED && code != 0) {
            snprintf(g_dlab.status, sizeof(g_dlab.status), "Button focus notify id=%u code=%u; no EndDialog", id, code);
            return TRUE;
        }
        if (id == IDOK || id == IDCANCEL) {
            char name[96];
            name[0] = 0;
            if (GetDlgItem(hDlg, DLAB_EDIT_NAME)) GetDlgItemTextA(hDlg, DLAB_EDIT_NAME, name, sizeof(name));
            else if (GetDlgItem(hDlg, DLAB_COMBO_ITEMS)) {
                int sel = (int)SendDlgItemMessageA(hDlg, DLAB_COMBO_ITEMS, CB_GETCURSEL, 0, 0);
                if (sel >= 0) SendDlgItemMessageA(hDlg, DLAB_COMBO_ITEMS, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)name);
            } else if (GetDlgItem(hDlg, DLAB_CHECK_AUTO)) {
                snprintf(name, sizeof(name), "auto=%u tri=%u r1=%u r2=%u r3=%u",
                         IsDlgButtonChecked(hDlg, DLAB_CHECK_AUTO),
                         IsDlgButtonChecked(hDlg, DLAB_CHECK_3STATE),
                         IsDlgButtonChecked(hDlg, DLAB_RADIO_ONE),
                         IsDlgButtonChecked(hDlg, DLAB_RADIO_TWO),
                         IsDlgButtonChecked(hDlg, DLAB_RADIO_THREE));
            } else if (GetDlgItem(hDlg, DLAB_EDIT_NORMAL)) {
                char normal[64] = {0};
                char multi[64] = {0};
                GetDlgItemTextA(hDlg, DLAB_EDIT_NORMAL, normal, sizeof(normal));
                GetDlgItemTextA(hDlg, DLAB_EDIT_MULTI, multi, sizeof(multi));
                snprintf(name, sizeof(name), "normal='%s' multi='%.24s'", normal, multi);
            } else if (GetDlgItem(hDlg, DLAB_KEY_NAME)) {
                char nm[48] = {0};
                GetDlgItemTextA(hDlg, DLAB_KEY_NAME, nm, sizeof(nm));
                snprintf(name, sizeof(name), "key='%s' radio=[%u,%u,%u]", nm,
                         IsDlgButtonChecked(hDlg, DLAB_KEY_R1),
                         IsDlgButtonChecked(hDlg, DLAB_KEY_R2),
                         IsDlgButtonChecked(hDlg, DLAB_KEY_R3));
            }
            dlab_result((INT_PTR)id, name);
            snprintf(g_dlab.status, sizeof(g_dlab.status), "DialogProc WM_COMMAND %s text='%s' -> EndDialog", id == IDOK ? "IDOK" : "IDCANCEL", name);
            if (hDlg == g_dlab.hControls) g_dlab.hControls = 0;
            if (hDlg == g_dlab.hButtons) g_dlab.hButtons = 0;
            if (hDlg == g_dlab.hText) g_dlab.hText = 0;
            if (hDlg == g_dlab.hKeyboard) g_dlab.hKeyboard = 0;
            if (hDlg == g_dlab.hModeless) g_dlab.hModeless = 0;
            if (hDlg == g_dlab.hModal) g_dlab.hModal = 0;
            EndDialog(hDlg, (INT_PTR)id);
            return TRUE;
        }
        break;
    }
    case WM_VSCROLL: {
        if ((HWND)lParam == GetDlgItem(hDlg, DLAB_SCROLL_VALUE)) {
            UINT code = LOWORD(wParam);
            int pos = (SHORT)HIWORD(wParam);
            snprintf(g_dlab.status, sizeof(g_dlab.status), "SCROLLBAR WM_VSCROLL: code=%u pos=%d SBM_GETPOS=%ld", code, pos, (long)SendDlgItemMessageA(hDlg, DLAB_SCROLL_VALUE, SBM_GETPOS, 0, 0));
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        dlab_result(IDCANCEL, g_dlab.lastName);
        snprintf(g_dlab.status, sizeof(g_dlab.status), "DialogProc WM_CLOSE -> IDCANCEL");
        if (hDlg == g_dlab.hControls) g_dlab.hControls = 0;
        if (hDlg == g_dlab.hButtons) g_dlab.hButtons = 0;
        if (hDlg == g_dlab.hText) g_dlab.hText = 0;
        if (hDlg == g_dlab.hKeyboard) g_dlab.hKeyboard = 0;
        if (hDlg == g_dlab.hModeless) g_dlab.hModeless = 0;
        if (hDlg == g_dlab.hModal) g_dlab.hModal = 0;
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static void* dlab_modal_thread(void* arg)
{
    (void)arg;
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    g_dlab.modalRunning = 1;
    snprintf(g_dlab.status, sizeof(g_dlab.status), "DialogBoxIndirectParamA entered: DLGTEMPLATE pointer, modal loop owns queue");
    LPCDLGTEMPLATEA tpl = FindDialogTemplateA("AccessProbeDialog");
    INT_PTR r = DialogBoxIndirectParamA(0, tpl, g_dlab.hWnd, dlab_dialog_proc, 0x89);
    dlab_result(r, g_dlab.lastName);
    g_dlab.modalRunning = 0;
    g_dlab.hModal = 0;
    snprintf(g_dlab.status, sizeof(g_dlab.status), "DialogBoxIndirectParamA returned %s (%ld)", g_dlab.lastResultText, (long)r);
    return NULL;
}

static void dlab_open_modal(void)
{
    if (g_dlab.modalRunning) {
        snprintf(g_dlab.status, sizeof(g_dlab.status), "Modal dialog already running");
        return;
    }
    pthread_create(&g_dlab.modalThread, NULL, dlab_modal_thread, NULL);
    pthread_detach(g_dlab.modalThread);
}

static void dlab_open_modeless(void)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    if (g_dlab.hModeless && IsWindow(g_dlab.hModeless)) {
        SetFocus(g_dlab.hModeless);
        snprintf(g_dlab.status, sizeof(g_dlab.status), "Modeless dialog already open hDlg=%u", (unsigned)g_dlab.hModeless);
        return;
    }
    dlab_register_templates();
    g_dlab.hModeless = CreateDialogParamA(0, "AccessProbeDialog", g_dlab.hWnd, dlab_dialog_proc, 0x189);
    snprintf(g_dlab.status, sizeof(g_dlab.status), "CreateDialogParamA(name->DLGTEMPLATE) -> hDlg=%u (parent remains enabled)", (unsigned)g_dlab.hModeless);
}

static void dlab_open_controls(void)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    if (g_dlab.hControls && IsWindow(g_dlab.hControls)) {
        SetFocus(g_dlab.hControls);
        snprintf(g_dlab.status, sizeof(g_dlab.status), "ControlProbe already open hDlg=%u", (unsigned)g_dlab.hControls);
        return;
    }
    dlab_register_templates();
    g_dlab.hControls = CreateDialogParamA(0, "ControlProbeDialog", g_dlab.hWnd, dlab_dialog_proc, 0x192);
    snprintf(g_dlab.status, sizeof(g_dlab.status), "CreateDialogParamA(ControlProbeDialog): LISTBOX/COMBOBOX/SCROLLBAR from DLGTEMPLATE atoms");
}


static void dlab_open_buttons(void)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    if (g_dlab.hButtons && IsWindow(g_dlab.hButtons)) {
        SetFocus(g_dlab.hButtons);
        snprintf(g_dlab.status, sizeof(g_dlab.status), "ButtonProbe already open hDlg=%u", (unsigned)g_dlab.hButtons);
        return;
    }
    dlab_register_templates();
    g_dlab.hButtons = CreateDialogParamA(0, "ButtonProbeDialog", g_dlab.hWnd, dlab_dialog_proc, 0x193);
    snprintf(g_dlab.status, sizeof(g_dlab.status), "CreateDialogParamA(ButtonProbeDialog): BS_GROUPBOX/CHECKBOX/RADIO/PUSHBUTTON from DLGTEMPLATE atoms");
}


static void dlab_open_text(void)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    if (g_dlab.hText && IsWindow(g_dlab.hText)) {
        SetFocus(g_dlab.hText);
        snprintf(g_dlab.status, sizeof(g_dlab.status), "TextProbe already open hDlg=%u", (unsigned)g_dlab.hText);
        return;
    }
    dlab_register_templates();
    g_dlab.hText = CreateDialogParamA(0, "TextProbeDialog", g_dlab.hWnd, dlab_dialog_proc, 0x194);
    snprintf(g_dlab.status, sizeof(g_dlab.status), "CreateDialogParamA(TextProbeDialog): STATIC/EDIT hardening from DLGTEMPLATE atoms");
}

static void dlab_open_keyboard(void)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    if (g_dlab.hKeyboard && IsWindow(g_dlab.hKeyboard)) {
        SetFocus(g_dlab.hKeyboard);
        snprintf(g_dlab.status, sizeof(g_dlab.status), "KeyboardProbe already open hDlg=%u", (unsigned)g_dlab.hKeyboard);
        return;
    }
    dlab_register_templates();
    g_dlab.hKeyboard = CreateDialogParamA(0, "KeyboardProbeDialog", g_dlab.hWnd, dlab_dialog_proc, 0x195);
    snprintf(g_dlab.status, sizeof(g_dlab.status), "CreateDialogParamA(KeyboardProbeDialog): IsDialogMessageA/WM_NEXTDLGCTL/DM_GETDEFID/Alt mnemonics");
}

static void dlab_menu_probe(void)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    if (!g_dlab.hMenuBar) {
        g_dlab.hMenuBar = CreateMenu();
        g_dlab.hMenuFile = CreatePopupMenu();
        g_dlab.hMenuRecent = CreatePopupMenu();
        g_dlab.hMenuView = CreatePopupMenu();
        g_dlab.hMenuOwnerDraw = CreatePopupMenu();

        AppendMenuA(g_dlab.hMenuRecent, MF_STRING, DLAB_MENU_RECENT_ONE, "Template &One");
        AppendMenuA(g_dlab.hMenuRecent, MF_STRING, DLAB_MENU_RECENT_TWO, "Template &Two");

        AppendMenuA(g_dlab.hMenuFile, MF_STRING, DLAB_MENU_FILE_OPEN, "&Open");
        AppendMenuA(g_dlab.hMenuFile, MF_POPUP, (UINT_PTR)g_dlab.hMenuRecent, "&Recent");
        AppendMenuA(g_dlab.hMenuFile, MF_SEPARATOR, 0, NULL);
        AppendMenuA(g_dlab.hMenuFile, MF_STRING, DLAB_MENU_FILE_EXIT, "E&xit");

        AppendMenuA(g_dlab.hMenuView, MF_STRING | MF_CHECKED, DLAB_MENU_VIEW_CHECK, "&Checked item");
        AppendMenuA(g_dlab.hMenuView, MF_STRING | MF_GRAYED, DLAB_MENU_VIEW_DISABLED, "&Disabled item");
        InsertMenuA(g_dlab.hMenuView, 1, MF_BYPOSITION | MF_STRING, 0x9706u, "Inserted by &position");
        ModifyMenuA(g_dlab.hMenuView, 1, MF_BYPOSITION | MF_STRING, 0x9706u, "Modified inserted item");

        AppendMenuA(g_dlab.hMenuOwnerDraw, MF_OWNERDRAW, DLAB_MENU_OWNERDRAW, "owner data");
        AppendMenuA(g_dlab.hMenuOwnerDraw, MF_STRING, 0x9707u, "Normal after ownerdraw");

        AppendMenuA(g_dlab.hMenuBar, MF_POPUP, (UINT_PTR)g_dlab.hMenuFile, "&File");
        AppendMenuA(g_dlab.hMenuBar, MF_POPUP, (UINT_PTR)g_dlab.hMenuView, "&View");
        AppendMenuA(g_dlab.hMenuBar, MF_POPUP, (UINT_PTR)g_dlab.hMenuOwnerDraw, "&OwnerDraw");
        SetMenu(g_dlab.hWnd, g_dlab.hMenuBar);
        DrawMenuBar(g_dlab.hWnd);
    }

    int mainCount = GetMenuItemCount(g_dlab.hMenuBar);
    HMENU sub0 = GetSubMenu(g_dlab.hMenuBar, 0);
    UINT firstId = GetMenuItemID(sub0, 0);
    UINT oldCheck = CheckMenuItem(g_dlab.hMenuView, DLAB_MENU_VIEW_CHECK, MF_BYCOMMAND | MF_UNCHECKED);
    CheckMenuItem(g_dlab.hMenuView, DLAB_MENU_VIEW_CHECK, MF_BYCOMMAND | MF_CHECKED);
    UINT ret = (UINT)TrackPopupMenu(g_dlab.hMenuOwnerDraw, TPM_RETURNCMD, 32, 32, 0, g_dlab.hWnd, NULL);
    TrackPopupMenu(g_dlab.hMenuOwnerDraw, 0, 32, 32, 0, g_dlab.hWnd, NULL);
    snprintf(g_dlab.status, sizeof(g_dlab.status),
             "MenuProbe: bar items=%d sub0=%u firstId=0x%x oldCheck=0x%x ownerdraw TrackPopup ret=0x%x",
             mainCount, (unsigned)sub0, firstId, oldCheck, ret);
}


static void dlab_open_file_common(BOOL saveDialog)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    char file[MAX_PATH];
    char title[MAX_PATH];
    memset(file, 0, sizeof(file));
    memset(title, 0, sizeof(title));
    if (saveDialog) snprintf(file, sizeof(file), "myos_saved_file.txt");

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_dlab.hWnd;
    ofn.lpstrFilter = "C Source (*.c;*.h)\0*.c;*.h\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrFileTitle = title;
    ofn.nMaxFileTitle = sizeof(title);
    ofn.lpstrInitialDir = ".";
    ofn.lpstrTitle = saveDialog ? "myOS GetSaveFileNameA Probe" : "myOS GetOpenFileNameA Probe";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!saveDialog) ofn.Flags |= OFN_FILEMUSTEXIST;
    else ofn.Flags |= OFN_OVERWRITEPROMPT;

    BOOL ok = saveDialog ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    DWORD err = CommDlgExtendedError();
    if (ok) {
        snprintf(g_dlab.status, sizeof(g_dlab.status), "%s returned TRUE file='%s' title='%s' filter=%u off=%u ext=%u",
                 saveDialog ? "GetSaveFileNameA" : "GetOpenFileNameA",
                 ofn.lpstrFile, ofn.lpstrFileTitle ? ofn.lpstrFileTitle : "", (unsigned)ofn.nFilterIndex,
                 (unsigned)ofn.nFileOffset, (unsigned)ofn.nFileExtension);
        snprintf(g_dlab.dump[0], sizeof(g_dlab.dump[0]), "OPENFILENAMEA: Flags=0x%08x nMaxFile=%u lpstrInitialDir='.'", (unsigned)ofn.Flags, (unsigned)ofn.nMaxFile);
        snprintf(g_dlab.dump[1], sizeof(g_dlab.dump[1]), "Selected file: %s", ofn.lpstrFile);
        snprintf(g_dlab.dump[2], sizeof(g_dlab.dump[2]), "Selected title: %s", ofn.lpstrFileTitle ? ofn.lpstrFileTitle : "");
        snprintf(g_dlab.dump[3], sizeof(g_dlab.dump[3]), "CommDlgExtendedError()=0x%lx", (unsigned long)err);
    } else {
        snprintf(g_dlab.status, sizeof(g_dlab.status), "%s returned FALSE CommDlgExtendedError=0x%lx",
                 saveDialog ? "GetSaveFileNameA" : "GetOpenFileNameA", (unsigned long)err);
        snprintf(g_dlab.dump[0], sizeof(g_dlab.dump[0]), "Common Dialog cancel/error path: lpstrFile='%s'", file);
        snprintf(g_dlab.dump[1], sizeof(g_dlab.dump[1]), "Filter format uses double-NUL pairs: display\\0pattern\\0...\\0\\0");
        snprintf(g_dlab.dump[2], sizeof(g_dlab.dump[2]), "Supported now: OFN_FILEMUSTEXIST/PATHMUSTEXIST/NOCHANGEDIR/HIDEREADONLY/OVERWRITEPROMPT");
        snprintf(g_dlab.dump[3], sizeof(g_dlab.dump[3]), "CommDlgExtendedError() returns 0 on cancel, FNERR/CDERR on validation/runtime errors");
    }
}


static void dlab_choose_font_dialog(void)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    LOGFONTA lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -10;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    snprintf(lf.lfFaceName, sizeof(lf.lfFaceName), "MS Shell Dlg");

    char style[32];
    snprintf(style, sizeof(style), "Regular");

    CHOOSEFONTA cf;
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_dlab.hWnd;
    cf.lpLogFont = &lf;
    cf.iPointSize = 100;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS | CF_LIMITSIZE | CF_USESTYLE;
    cf.rgbColors = RGB(80,120,255);
    cf.lpszStyle = style;
    cf.nSizeMin = 8;
    cf.nSizeMax = 36;

    BOOL ok = ChooseFontA(&cf);
    DWORD err = CommDlgExtendedError();
    if (ok) {
        snprintf(g_dlab.status, sizeof(g_dlab.status),
                 "ChooseFontA returned TRUE face='%s' style='%s' height=%ld weight=%ld pt=%d color=0x%06lx",
                 lf.lfFaceName, style, (long)lf.lfHeight, (long)lf.lfWeight, cf.iPointSize / 10,
                 (unsigned long)cf.rgbColors & 0xFFFFFFul);
        snprintf(g_dlab.dump[0], sizeof(g_dlab.dump[0]), "CHOOSEFONTA: Flags=0x%08x nFontType=0x%04x sizeMin=%d sizeMax=%d",
                 (unsigned)cf.Flags, (unsigned)cf.nFontType, cf.nSizeMin, cf.nSizeMax);
        snprintf(g_dlab.dump[1], sizeof(g_dlab.dump[1]), "LOGFONTA: face='%s' height=%ld weight=%ld italic=%u underline=%u strikeout=%u",
                 lf.lfFaceName, (long)lf.lfHeight, (long)lf.lfWeight, lf.lfItalic, lf.lfUnderline, lf.lfStrikeOut);
        snprintf(g_dlab.dump[2], sizeof(g_dlab.dump[2]), "Style buffer='%s' iPointSize=%d tenths-of-a-point", style, cf.iPointSize);
        snprintf(g_dlab.dump[3], sizeof(g_dlab.dump[3]), "CommDlgExtendedError()=0x%lx", (unsigned long)err);
    } else {
        snprintf(g_dlab.status, sizeof(g_dlab.status), "ChooseFontA returned FALSE CommDlgExtendedError=0x%lx", (unsigned long)err);
        snprintf(g_dlab.dump[0], sizeof(g_dlab.dump[0]), "ChooseFontA cancel/error path: face='%s' style='%s'", lf.lfFaceName, style);
        snprintf(g_dlab.dump[1], sizeof(g_dlab.dump[1]), "Supported now: CF_SCREENFONTS/INITTOLOGFONTSTRUCT/EFFECTS/LIMITSIZE/FIXEDPITCHONLY/USESTYLE");
        snprintf(g_dlab.dump[2], sizeof(g_dlab.dump[2]), "Returns LOGFONTA, rgbColors, nFontType, iPointSize and lpszStyle");
        snprintf(g_dlab.dump[3], sizeof(g_dlab.dump[3]), "CommDlgExtendedError() returns 0 on cancel, CFERR/CDERR on validation/runtime errors");
    }
}

static void dlab_open_file_dialog(void)
{
    dlab_open_file_common(FALSE);
}

static void dlab_save_file_dialog(void)
{
    dlab_open_file_common(TRUE);
}

static void dlab_enable_standard_scrollbars(void)
{
    MyWinBindRuntime(g_dlab.mgr, &g_dlab.cap);
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = 200; si.nPage = 30; si.nPos = 40;
    ShowScrollBar(g_dlab.hWnd, SB_BOTH, TRUE);
    SetScrollInfo(g_dlab.hWnd, SB_VERT, &si, TRUE);
    si.nMax = 120; si.nPage = 20; si.nPos = 10;
    SetScrollInfo(g_dlab.hWnd, SB_HORZ, &si, TRUE);
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si); si.fMask = SIF_ALL;
    GetScrollInfo(g_dlab.hWnd, SB_VERT, &si);
    snprintf(g_dlab.status, sizeof(g_dlab.status), "Std WS_VSCROLL/WS_HSCROLL enabled: GetScrollInfo V range=%d..%d page=%u pos=%d", si.nMin, si.nMax, si.nPage, si.nPos);
}

static void dlab_handle_command(UINT cmd)
{
    switch (cmd) {
    case DLAB_CMD_MODAL:    dlab_open_modal(); break;
    case DLAB_CMD_MODELESS: dlab_open_modeless(); break;
    case DLAB_CMD_DUMP:     dlab_dump_template(); break;
    case DLAB_CMD_CONTROLS: dlab_open_controls(); break;
    case DLAB_CMD_BUTTONS:  dlab_open_buttons(); break;
    case DLAB_CMD_TEXT:     dlab_open_text(); break;
    case DLAB_CMD_KEYBOARD: dlab_open_keyboard(); break;
    case DLAB_CMD_SCROLLSTD: dlab_enable_standard_scrollbars(); break;
    case DLAB_CMD_MENU: dlab_menu_probe(); break;
    case DLAB_CMD_DUMPNAV: dlab_dump_nav(); break;
    case DLAB_CMD_OPENFILE: dlab_open_file_dialog(); break;
    case DLAB_CMD_SAVEFILE: dlab_save_file_dialog(); break;
    case DLAB_CMD_CHOOSEFONT: dlab_choose_font_dialog(); break;
    default: break;
    }
}

static LRESULT CALLBACK dialoglab_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_CREATE:
        g_dlab.hWnd = hWnd;
        dlab_register_templates();
        MyAppResizeInit(&g_dlab.resize, DIALOGLAB_W, DIALOGLAB_H, TITLEBAR_H);
        dlab_result(IDCANCEL, "");
        snprintf(g_dlab.status, sizeof(g_dlab.status), "DialogLab v119: app/lab breakage audit baseline");
        dlab_dump_template();
        return 0;
    case WM_LBUTTONDOWN:
        if (!IsWindowEnabled(hWnd)) return 0;
        dlab_click(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) >= 0x9700u && LOWORD(wParam) < 0x9800u) {
            snprintf(g_dlab.status, sizeof(g_dlab.status), "WM_COMMAND from menu id=0x%04x lParam=0x%lx", LOWORD(wParam), (long)lParam);
            return 0;
        }
        dlab_handle_command(LOWORD(wParam));
        return 0;
    case WM_INITMENU:
        snprintf(g_dlab.status, sizeof(g_dlab.status), "WM_INITMENU hMenu=0x%lx", (long)wParam);
        return 0;
    case WM_INITMENUPOPUP:
        snprintf(g_dlab.status, sizeof(g_dlab.status), "WM_INITMENUPOPUP hMenu=0x%lx pos=%u", (long)wParam, LOWORD(lParam));
        return 0;
    case WM_MENUSELECT:
        snprintf(g_dlab.status, sizeof(g_dlab.status), "WM_MENUSELECT id=0x%04x flags=0x%04x hMenu=0x%lx", LOWORD(wParam), HIWORD(wParam), (long)lParam);
        return 0;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lParam;
        if (mi && mi->CtlType == ODT_MENU) {
            mi->itemWidth = 172;
            mi->itemHeight = 20;
            snprintf(g_dlab.status, sizeof(g_dlab.status), "WM_MEASUREITEM ODT_MENU id=0x%x -> %ux%u", mi->itemID, mi->itemWidth, mi->itemHeight);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lParam;
        if (di && di->CtlType == ODT_MENU) {
            snprintf(g_dlab.status, sizeof(g_dlab.status), "WM_DRAWITEM ODT_MENU id=0x%x rc=(%d,%d,%d,%d)", di->itemID, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom);
            return TRUE;
        }
        break;
    }
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_dlab.resize, lParam, DIALOGLAB_MIN_W, DIALOGLAB_MIN_H);
        return 0;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_dlab.resize, lParam);
        return 0;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_dlab.resize, lParam, TITLEBAR_H);
        g_dlab.signalCount++;
        return 0;
    case WM_MOVE:
        MyAppResizeOnMove(&g_dlab.resize, lParam);
        return 0;
    case WM_SIZE:
        MyAppResizeOnSize(&g_dlab.resize, wParam, lParam);
        g_dlab.signalCount++;
        return 0;
    case WM_DESTROY:
        if (g_dlab.hModeless && IsWindow(g_dlab.hModeless)) EndDialog(g_dlab.hModeless, IDCANCEL);
        if (g_dlab.hControls && IsWindow(g_dlab.hControls)) EndDialog(g_dlab.hControls, IDCANCEL);
        if (g_dlab.hButtons && IsWindow(g_dlab.hButtons)) EndDialog(g_dlab.hButtons, IDCANCEL);
        if (g_dlab.hText && IsWindow(g_dlab.hText)) EndDialog(g_dlab.hText, IDCANCEL);
        if (g_dlab.hKeyboard && IsWindow(g_dlab.hKeyboard)) EndDialog(g_dlab.hKeyboard, IDCANCEL);
        if (g_dlab.hModal && IsWindow(g_dlab.hModal)) EndDialog(g_dlab.hModal, IDCANCEL);
        if (g_dlab.hMenuBar) DestroyMenu(g_dlab.hMenuBar);
        g_dlab.hModeless = 0;
        g_dlab.hControls = 0;
        g_dlab.hButtons = 0;
        g_dlab.hText = 0;
        g_dlab.hKeyboard = 0;
        g_dlab.hModal = 0;
        g_dlab.hMenuBar = g_dlab.hMenuFile = g_dlab.hMenuView = g_dlab.hMenuOwnerDraw = 0;
        g_dlab.modalRunning = 0;
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

static ATOM dialoglab_register_class(void)
{
    static ATOM s_atom = 0;
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = dialoglab_wndproc;
    wc.lpszClassName = "myOS.DialogLab";
    s_atom = RegisterClassExA(&wc);
    return s_atom;
}

HWND dialoglab_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    memset(&g_dlab, 0, sizeof(g_dlab));
    g_dlab.mgr = mgr;
    g_dlab.cap = cap;
    dlab_result(IDCANCEL, "");
    MyWinBindRuntime(mgr, &cap);
    dialoglab_register_class();
    HWND hWnd = CreateWindowExA(WS_EX_NONE, "myOS.DialogLab", "myOS DialogLab",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                x, y, DIALOGLAB_W, DIALOGLAB_H, 0, 0, 0, NULL);
    g_dlab.hWnd = hWnd;
    return hWnd;
}

void dialoglab_blit(HWND hwnd, int x, int y, int w, int h, Framebuffer* fb)
{
    if (!fb) return;
    if (hwnd && g_dlab.hWnd != hwnd) g_dlab.hWnd = hwnd;
    int cx = x + 1;
    int cy = y + TITLEBAR_H;
    int cw = w - 2;
    int ch = h - TITLEBAR_H - 1;
    if (cw < 80 || ch < 60) return;

    fb_rect(fb, cx, cy, cw, ch, IsWindowEnabled(g_dlab.hWnd) ? COLOR(12,14,22) : COLOR(18,18,18));
    fb_rect_outline(fb, cx, cy, cw, ch, COLOR(65,75,105));

    dlab_button(fb, cx + 14, cy + 12, 160, "Open Modal Dialog");
    dlab_button(fb, cx + 184, cy + 12, 176, "Open Modeless Dialog");
    dlab_button(fb, cx + 370, cy + 12, 130, "Dump Template");
    dlab_button(fb, cx + 510, cy + 12, 170, "Open Controls Dialog");
    dlab_button(fb, cx + 690, cy + 12, 160, "Open Button Dialog");
    dlab_button(fb, cx + 14, cy + 42, 160, "Open Text Dialog");
    dlab_button(fb, cx + 184, cy + 42, 176, "Open Keyboard Dialog");
    dlab_button(fb, cx + 370, cy + 42, 180, "Enable Std Scrollbars");
    dlab_button(fb, cx + 560, cy + 42, 170, "Menu APIs Probe");
    dlab_button(fb, cx + 740, cy + 42, 160, "Dump Dialog Nav");
    dlab_button(fb, cx + 14, cy + 72, 200, "GetOpenFileNameA");
    dlab_button(fb, cx + 224, cy + 72, 200, "GetSaveFileNameA");
    dlab_button(fb, cx + 434, cy + 72, 160, "ChooseFontA");

    char line[260];
    snprintf(line, sizeof(line), "Last result: %s   parent=%s",
             g_dlab.lastResultText,
             IsWindowEnabled(g_dlab.hWnd) ? "enabled" : "disabled by modal");
    DrawClipTextA(fb, cx + 14, cy + 104, line, COLOR(190,235,210), cx + 10, cy + 100, cw - 20, 14);

    snprintf(line, sizeof(line), "Tests: DLGTEMPLATE; Dialog keyboard; Menus; GetOpen/Save/ChooseFont");
    DrawClipTextA(fb, cx + 14, cy + 126, line, COLOR(220,220,235), cx + 10, cy + 122, cw - 20, 14);

    snprintf(line, sizeof(line), "status: %s", g_dlab.status);
    DrawClipTextA(fb, cx + 14, cy + 148, line, COLOR(170,210,255), cx + 10, cy + 144, cw - 20, 14);

    snprintf(line, sizeof(line), "HWND parent=%u modeless=%u controls=%u buttons=%u text=%u keyboard=%u modalRunning=%d signals=%d",
             (unsigned)g_dlab.hWnd, (unsigned)g_dlab.hModeless, (unsigned)g_dlab.hControls, (unsigned)g_dlab.hButtons, (unsigned)g_dlab.hText, (unsigned)g_dlab.hKeyboard, g_dlab.modalRunning, g_dlab.signalCount);
    DrawClipTextA(fb, cx + 14, cy + 170, line, COLOR(210,190,255), cx + 10, cy + 166, cw - 20, 14);

    for (int i = 0; i < 4; ++i) {
        if (g_dlab.dump[i][0])
            DrawClipTextA(fb, cx + 14, cy + 194 + i * 18, g_dlab.dump[i], COLOR(200,210,225), cx + 10, cy + 190 + i * 18, cw - 20, 14);
    }

    /* v90: Dialog controls are no longer hardcoded in USER32. DialogLab
       registers a real DLGTEMPLATE resource and USER32 builds children from it. */
    MyDrawChildWindows(g_dlab.hWnd, fb, cx, cy);

}
