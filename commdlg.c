#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>

// v118: sdk/include/commdlg.h -> commdlg.c
// Public COMDLG32/MSDN entrypoints live here directly.
// Dialog rendering still goes through the public USER32 surface.

/* ──────────────────────────────────────────────────────────────────────
   v118 COMDLG32: GetOpenFileNameA / GetSaveFileNameA

   This intentionally uses the public USER32 dialog/control surface instead
   of a one-off renderer: DLGTEMPLATE, #32770, EDIT, LISTBOX, COMBOBOX,
   BUTTON, WM_COMMAND, IsDialogMessageA and EndDialog are all exercised here.
   ────────────────────────────────────────────────────────────────────── */

static __thread DWORD g_CommDlgExtendedError = 0;

#define MYWIN_CDLG_ID_PATH       0x3E81u
#define MYWIN_CDLG_ID_LIST       0x3E82u
#define MYWIN_CDLG_ID_FILENAME   0x3E83u
#define MYWIN_CDLG_ID_FILTER     0x3E84u
#define MYWIN_CDLG_ID_READONLY   0x3E85u
#define MYWIN_CDLG_ID_STATUS     0x3E86u

#define MYWIN_CDLG_MAX_FILTERS 16
#define MYWIN_CDLG_MAX_FILES   128
#define MYWIN_CDLG_TEXT_CHARS   96

typedef struct MyCommDlgFilterA {
    char display[96];
    char pattern[128];
} MyCommDlgFilterA;

typedef struct MyCommDlgFileDialogStateA {
    LPOPENFILENAMEA lpofn;
    BOOL saveDialog;
    char oldDir[MAX_PATH];
    char currentDir[MAX_PATH];
    MyCommDlgFilterA filters[MYWIN_CDLG_MAX_FILTERS];
    int filterCount;
    int currentFilter;
    char listNames[MYWIN_CDLG_MAX_FILES][MAX_PATH];
    unsigned char listIsDir[MYWIN_CDLG_MAX_FILES];
    int listCount;
    char pendingOverwrite[MAX_PATH];
    char runtimeDefExt[32];
    unsigned notifyInitDone;
    unsigned notifySelChange;
    unsigned notifyFolderChange;
    unsigned notifyFileOk;
    unsigned notifyTypeChange;
    unsigned cdmQueries;
} MyCommDlgFileDialogStateA;

static void mywin_commdlg_set_error(DWORD err)
{
    g_CommDlgExtendedError = err;
    SetLastError(err == 0 ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER);
}

DWORD CommDlgExtendedError(void)
{
    return g_CommDlgExtendedError;
}

static void mywin_commdlg_mark_buffer_too_small(LPOPENFILENAMEA ofn, size_t needChars)
{
    if (!ofn || !ofn->lpstrFile) return;
    WORD need = (needChars > 0xFFFFu) ? 0xFFFFu : (WORD)needChars;
    if (ofn->nMaxFile >= sizeof(WORD)) memcpy(ofn->lpstrFile, &need, sizeof(need));
}

static BOOL mywin_commdlg_file_supported_or_error(LPOPENFILENAMEA ofn)
{
    if (!ofn) return FALSE;

    /* v196 compliance guard: hook-enabled common dialogs must either provide a
       hook proc that we can call or fail with CDERR_NOHOOK. Template/resource
       variants need a resource loader/custom-template merge path; silently
       ignoring them would violate the Win32 contract, so fail deterministically
       instead of displaying the wrong dialog. */
    if ((ofn->Flags & OFN_ENABLEHOOK) && !ofn->lpfnHook) {
        mywin_commdlg_set_error(CDERR_NOHOOK);
        return FALSE;
    }
    if ((ofn->Flags & OFN_ENABLETEMPLATE) && !ofn->hInstance) {
        mywin_commdlg_set_error(CDERR_NOHINSTANCE);
        return FALSE;
    }
    if ((ofn->Flags & OFN_ENABLETEMPLATE) && !ofn->lpTemplateName) {
        mywin_commdlg_set_error(CDERR_NOTEMPLATE);
        return FALSE;
    }
    if ((ofn->Flags & OFN_ENABLETEMPLATEHANDLE) && !ofn->hInstance) {
        mywin_commdlg_set_error(CDERR_NOTEMPLATE);
        return FALSE;
    }
    return TRUE;
}

static BOOL mywin_commdlg_font_supported_or_error(LPCHOOSEFONTA cf)
{
    if (!cf) return FALSE;
    if ((cf->Flags & CF_ENABLEHOOK) && !cf->lpfnHook) {
        mywin_commdlg_set_error(CDERR_NOHOOK);
        return FALSE;
    }
    if ((cf->Flags & CF_ENABLETEMPLATE) && !cf->hInstance) {
        mywin_commdlg_set_error(CDERR_NOHINSTANCE);
        return FALSE;
    }
    if ((cf->Flags & CF_ENABLETEMPLATE) && !cf->lpTemplateName) {
        mywin_commdlg_set_error(CDERR_NOTEMPLATE);
        return FALSE;
    }
    if ((cf->Flags & CF_ENABLETEMPLATEHANDLE) && !cf->hInstance) {
        mywin_commdlg_set_error(CDERR_NOTEMPLATE);
        return FALSE;
    }
    if ((cf->Flags & CF_USESTYLE) && !cf->lpszStyle) {
        mywin_commdlg_set_error(CDERR_INITIALIZATION);
        return FALSE;
    }
    return TRUE;
}


typedef struct MyCommDlgResolvedTemplateA {
    LPCDLGTEMPLATEA lpTemplate;
    HGLOBAL hGlobalUnlock;
} MyCommDlgResolvedTemplateA;

static void mywin_commdlg_release_template(MyCommDlgResolvedTemplateA* rt)
{
    if (!rt) return;
    if (rt->hGlobalUnlock) GlobalUnlock(rt->hGlobalUnlock);
    memset(rt, 0, sizeof(*rt));
}

static BOOL mywin_commdlg_resolve_template_handle(HGLOBAL hTemplate, MyCommDlgResolvedTemplateA* out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    if (!hTemplate) { mywin_commdlg_set_error(CDERR_NOTEMPLATE); return FALSE; }

    /* First try resource data handles returned by LoadResource.  If that fails,
       try normal movable memory handles.  This keeps HINSTANCE/HGLOBAL 32-bit
       opaque and avoids treating handles as raw process pointers. */
    LPVOID p = LockResource(hTemplate);
    if (p) {
        out->lpTemplate = (LPCDLGTEMPLATEA)p;
        return TRUE;
    }
    p = GlobalLock(hTemplate);
    if (!p) { mywin_commdlg_set_error(CDERR_LOCKRESFAILURE); return FALSE; }
    out->lpTemplate = (LPCDLGTEMPLATEA)p;
    out->hGlobalUnlock = hTemplate;
    return TRUE;
}

static BOOL mywin_commdlg_resolve_resource_template(HINSTANCE hInstance, LPCSTR lpTemplateName, MyCommDlgResolvedTemplateA* out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    if (!hInstance) { mywin_commdlg_set_error(CDERR_NOHINSTANCE); return FALSE; }
    if (!lpTemplateName) { mywin_commdlg_set_error(CDERR_NOTEMPLATE); return FALSE; }
    HRSRC hRes = FindResourceA((HMODULE)hInstance, lpTemplateName, RT_DIALOG);
    if (!hRes) { mywin_commdlg_set_error(CDERR_FINDRESFAILURE); return FALSE; }
    HGLOBAL hData = LoadResource((HMODULE)hInstance, hRes);
    if (!hData) { mywin_commdlg_set_error(CDERR_LOADRESFAILURE); return FALSE; }
    LPVOID p = LockResource(hData);
    if (!p) { mywin_commdlg_set_error(CDERR_LOCKRESFAILURE); return FALSE; }
    out->lpTemplate = (LPCDLGTEMPLATEA)p;
    return TRUE;
}

static BOOL mywin_commdlg_select_template(HINSTANCE hInstance, LPCSTR lpTemplateName,
                                          DWORD flags, DWORD enableTemplate, DWORD enableTemplateHandle,
                                          LPCDLGTEMPLATEA builtinTemplate,
                                          MyCommDlgResolvedTemplateA* out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    if ((flags & enableTemplate) && (flags & enableTemplateHandle)) {
        mywin_commdlg_set_error(CDERR_INITIALIZATION);
        return FALSE;
    }
    if (flags & enableTemplateHandle)
        return mywin_commdlg_resolve_template_handle((HGLOBAL)hInstance, out);
    if (flags & enableTemplate)
        return mywin_commdlg_resolve_resource_template(hInstance, lpTemplateName, out);
    out->lpTemplate = builtinTemplate;
    return out->lpTemplate != NULL;
}

static void mywin_path_normalize_slashes(char* s)
{
    if (!s) return;
    for (; *s; ++s) if (*s == '\\') *s = '/';
}

static BOOL mywin_path_is_absolute(LPCSTR s)
{
    if (!s || !s[0]) return FALSE;
    if (s[0] == '/') return TRUE;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') return TRUE;
    return FALSE;
}

static void mywin_path_join(char* out, size_t cb, LPCSTR dir, LPCSTR name)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    if (!name) name = "";
    if (mywin_path_is_absolute(name)) {
        snprintf(out, cb, "%s", name);
    } else {
        if (!dir || !dir[0]) dir = ".";
        size_t n = strlen(dir);
        snprintf(out, cb, "%s%s%s", dir, (n > 0 && dir[n-1] == '/') ? "" : "/", name);
    }
    mywin_path_normalize_slashes(out);
}

static void mywin_path_dirname(LPCSTR path, char* out, size_t cb)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    if (!path || !path[0]) { snprintf(out, cb, "."); return; }
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    mywin_path_normalize_slashes(tmp);
    char* slash = strrchr(tmp, '/');
    if (!slash) { snprintf(out, cb, "."); return; }
    if (slash == tmp) { snprintf(out, cb, "/"); return; }
    *slash = 0;
    snprintf(out, cb, "%s", tmp);
}

static LPCSTR mywin_path_basename_local(LPCSTR path)
{
    if (!path) return "";
    LPCSTR a = strrchr(path, '/');
    LPCSTR b = strrchr(path, '\\');
    LPCSTR p = a > b ? a : b;
    return p ? p + 1 : path;
}

static LPCSTR mywin_path_extension_local(LPCSTR path)
{
    LPCSTR base = mywin_path_basename_local(path);
    LPCSTR dot = strrchr(base, '.');
    if (!dot || dot == base || dot[1] == 0) return NULL;
    return dot + 1;
}

static void mywin_commdlg_append_default_extension_ex(LPOPENFILENAMEA ofn, LPCSTR defExtOverride, char* full, size_t cb)
{
    LPCSTR defExt = (defExtOverride && defExtOverride[0]) ? defExtOverride : (ofn ? ofn->lpstrDefExt : NULL);
    if (!ofn || !full || cb == 0 || !defExt || !defExt[0]) return;
    if (mywin_path_extension_local(full)) return;
    LPCSTR ext = defExt;
    while (*ext == '.') ext++;
    if (!*ext) return;
    size_t n = strlen(full);
    if (n + 1 + strlen(ext) + 1 >= cb) return;
    full[n++] = '.';
    snprintf(full + n, cb - n, "%s", ext);
}

static void mywin_commdlg_append_default_extension(LPOPENFILENAMEA ofn, char* full, size_t cb)
{
    mywin_commdlg_append_default_extension_ex(ofn, NULL, full, cb);
}

static void mywin_commdlg_update_extension_flag(LPOPENFILENAMEA ofn, LPCSTR full)
{
    if (!ofn || !ofn->lpstrDefExt || !ofn->lpstrDefExt[0]) return;
    LPCSTR chosen = mywin_path_extension_local(full);
    LPCSTR def = ofn->lpstrDefExt;
    while (*def == '.') def++;
    if (chosen && *def && strcasecmp(chosen, def) != 0) ofn->Flags |= OFN_EXTENSIONDIFFERENT;
    else ofn->Flags &= ~OFN_EXTENSIONDIFFERENT;
}

static BOOL mywin_path_exists(LPCSTR path, BOOL* isDir)
{
    struct stat st;
    if (isDir) *isDir = FALSE;
    if (!path || !path[0]) return FALSE;
    if (stat(path, &st) != 0) return FALSE;
    if (isDir) *isDir = S_ISDIR(st.st_mode) ? TRUE : FALSE;
    return TRUE;
}

static int mywin_ascii_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

static BOOL mywin_wildmatch_ci(LPCSTR pat, LPCSTR text)
{
    if (!pat || !pat[0]) pat = "*";
    if (!text) text = "";
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return TRUE;
            while (*text) {
                if (mywin_wildmatch_ci(pat, text)) return TRUE;
                text++;
            }
            return mywin_wildmatch_ci(pat, text);
        }
        if (*pat == '?') {
            if (!*text) return FALSE;
            pat++; text++;
            continue;
        }
        if (mywin_ascii_tolower((unsigned char)*pat) != mywin_ascii_tolower((unsigned char)*text))
            return FALSE;
        pat++; text++;
    }
    return *text == 0;
}

static BOOL mywin_filter_match_one(LPCSTR patterns, LPCSTR filename)
{
    if (!patterns || !patterns[0] || strcmp(patterns, "*.*") == 0 || strcmp(patterns, "*") == 0)
        return TRUE;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", patterns);
    char* save = NULL;
    for (char* tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        char* end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1])) *--end = 0;
        if (mywin_wildmatch_ci(tok, filename)) return TRUE;
    }
    return FALSE;
}

static void mywin_commdlg_parse_filters(MyCommDlgFileDialogStateA* st)
{
    st->filterCount = 0;
    st->currentFilter = 0;
    LPCSTR p = st->lpofn ? st->lpofn->lpstrFilter : NULL;
    if (p) {
        while (*p && st->filterCount < MYWIN_CDLG_MAX_FILTERS) {
            LPCSTR display = p;
            p += strlen(p) + 1;
            if (!*p) break;
            LPCSTR pattern = p;
            p += strlen(p) + 1;
            snprintf(st->filters[st->filterCount].display, sizeof(st->filters[st->filterCount].display), "%s", display);
            snprintf(st->filters[st->filterCount].pattern, sizeof(st->filters[st->filterCount].pattern), "%s", pattern);
            st->filterCount++;
        }
    }
    if (st->filterCount == 0) {
        snprintf(st->filters[0].display, sizeof(st->filters[0].display), "All Files (*.*)");
        snprintf(st->filters[0].pattern, sizeof(st->filters[0].pattern), "*.*");
        st->filterCount = 1;
    }
    DWORD n = st->lpofn ? st->lpofn->nFilterIndex : 0;
    if (n >= 1 && n <= (DWORD)st->filterCount) st->currentFilter = (int)n - 1;
}

static void mywin_commdlg_set_status(HWND hDlg, LPCSTR text)
{
    SetDlgItemTextA(hDlg, MYWIN_CDLG_ID_STATUS, text ? text : "");
}

static void mywin_commdlg_fill_filters(HWND hDlg, MyCommDlgFileDialogStateA* st)
{
    SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_FILTER, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < st->filterCount; ++i)
        SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_FILTER, CB_ADDSTRING, 0, (LPARAM)st->filters[i].display);
    SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_FILTER, CB_SETCURSEL, (WPARAM)st->currentFilter, 0);
    /* v118: no private USER32 struct reach-through here; COMDLG talks through public control messages only. */
}

static void mywin_commdlg_fill_file_list(HWND hDlg, MyCommDlgFileDialogStateA* st)
{
    st->listCount = 0;
    SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_LIST, LB_RESETCONTENT, 0, 0);
    SetDlgItemTextA(hDlg, MYWIN_CDLG_ID_PATH, st->currentDir);

    int idx = (int)SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_LIST, LB_ADDSTRING, 0, (LPARAM)"[..]");
    if (idx >= 0) SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_LIST, LB_SETITEMDATA, (WPARAM)idx, 1);
    snprintf(st->listNames[st->listCount], sizeof(st->listNames[st->listCount]), "..");
    st->listIsDir[st->listCount++] = 1;

    DIR* d = opendir(st->currentDir[0] ? st->currentDir : ".");
    if (!d) {
        mywin_commdlg_set_status(hDlg, "Cannot open directory");
        return;
    }

    struct dirent* de;
    while ((de = readdir(d)) != NULL && st->listCount < MYWIN_CDLG_MAX_FILES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char full[MAX_PATH];
        mywin_path_join(full, sizeof(full), st->currentDir, de->d_name);
        BOOL isDir = FALSE;
        if (!mywin_path_exists(full, &isDir)) continue;
        if (!isDir && !mywin_filter_match_one(st->filters[st->currentFilter].pattern, de->d_name)) continue;

        char shown[80];
        size_t showLen = strlen(de->d_name);
        if (showLen >= sizeof(shown)) showLen = sizeof(shown) - 1;
        memcpy(shown, de->d_name, showLen);
        shown[showLen] = 0;
        char text[MYWIN_CDLG_TEXT_CHARS];
        if (isDir) snprintf(text, sizeof(text), "[%s]", shown);
        else snprintf(text, sizeof(text), "%s", shown);
        idx = (int)SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_LIST, LB_ADDSTRING, 0, (LPARAM)text);
        if (idx >= 0) SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_LIST, LB_SETITEMDATA, (WPARAM)idx, isDir ? 1 : 0);
        snprintf(st->listNames[st->listCount], sizeof(st->listNames[st->listCount]), "%s", de->d_name);
        st->listIsDir[st->listCount] = isDir ? 1 : 0;
        st->listCount++;
    }
    closedir(d);
    char msg[128];
    snprintf(msg, sizeof(msg), "%d visible item(s), filter: %s", st->listCount, st->filters[st->currentFilter].pattern);
    mywin_commdlg_set_status(hDlg, msg);
}

static size_t mywin_commdlg_copy_ansi(LPSTR out, size_t cch, LPCSTR text)
{
    size_t need = text ? strlen(text) + 1 : 1;
    if (out && cch > 0) {
        snprintf(out, cch, "%s", text ? text : "");
    }
    return need;
}

static void mywin_commdlg_get_spec(HWND hDlg, MyCommDlgFileDialogStateA* st, char* out, size_t cb)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    GetDlgItemTextA(hDlg, MYWIN_CDLG_ID_FILENAME, out, cb);
    if (!out[0] && st) {
        int sel = (int)SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_LIST, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < st->listCount) snprintf(out, cb, "%s", st->listNames[sel]);
    }
}

static void mywin_commdlg_get_current_full_path(HWND hDlg, MyCommDlgFileDialogStateA* st, char* out, size_t cb)
{
    char spec[MAX_PATH];
    mywin_commdlg_get_spec(hDlg, st, spec, sizeof(spec));
    mywin_path_join(out, cb, st ? st->currentDir : ".", spec);
    if (st && st->runtimeDefExt[0]) mywin_commdlg_append_default_extension_ex(st->lpofn, st->runtimeDefExt, out, cb);
    else if (st) mywin_commdlg_append_default_extension(st->lpofn, out, cb);
}

static INT_PTR mywin_commdlg_file_notify(MyCommDlgFileDialogStateA* st, HWND hDlg, UINT code, LPCSTR pszFile)
{
    if (!st || !st->lpofn || !(st->lpofn->Flags & OFN_ENABLEHOOK) || !st->lpofn->lpfnHook) return 0;
    OFNOTIFYA no;
    memset(&no, 0, sizeof(no));
    no.hdr.hwndFrom = hDlg;
    no.hdr.idFrom = 0;
    no.hdr.code = code;
    no.lpOFN = st->lpofn;
    no.pszFile = (LPSTR)(pszFile ? pszFile : "");
    if (code == CDN_INITDONE) st->notifyInitDone++;
    else if (code == CDN_SELCHANGE) st->notifySelChange++;
    else if (code == CDN_FOLDERCHANGE) st->notifyFolderChange++;
    else if (code == CDN_FILEOK) st->notifyFileOk++;
    else if (code == CDN_TYPECHANGE) st->notifyTypeChange++;
    return (INT_PTR)st->lpofn->lpfnHook(hDlg, WM_NOTIFY, 0, (LPARAM)&no);
}

static BOOL mywin_commdlg_multiselect_has_delim(LPCSTR s)
{
    if (!s) return FALSE;
    return strchr(s, ';') || strchr(s, '|') || strchr(s, '\n');
}

static BOOL mywin_commdlg_try_multiselect(HWND hDlg, MyCommDlgFileDialogStateA* st, LPCSTR name)
{
    if (!st || !st->lpofn || !(st->lpofn->Flags & OFN_ALLOWMULTISELECT) || !mywin_commdlg_multiselect_has_delim(name))
        return FALSE;
    char work[MAX_PATH * 2];
    snprintf(work, sizeof(work), "%s", name ? name : "");
    char* toks[64];
    int ntok = 0;
    char* p = work;
    while (*p && ntok < 64) {
        while (*p == ';' || *p == '|' || *p == '\n' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char* start = p;
        while (*p && *p != ';' && *p != '|' && *p != '\n') p++;
        if (*p) *p++ = 0;
        char* end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1])) *--end = 0;
        if (*start) toks[ntok++] = start;
    }
    if (ntok <= 1) return FALSE;

    size_t need = strlen(st->currentDir) + 1 + 1;
    for (int i = 0; i < ntok; ++i) need += strlen(mywin_path_basename_local(toks[i])) + 1;
    if (!st->lpofn->lpstrFile || st->lpofn->nMaxFile < need) {
        mywin_commdlg_mark_buffer_too_small(st->lpofn, need);
        mywin_commdlg_set_error(FNERR_BUFFERTOOSMALL);
        mywin_commdlg_set_status(hDlg, "lpstrFile buffer too small for multi-select");
        return TRUE;
    }

    for (int i = 0; i < ntok; ++i) {
        char full[MAX_PATH];
        mywin_path_join(full, sizeof(full), st->currentDir, toks[i]);
        mywin_commdlg_append_default_extension_ex(st->lpofn, st->runtimeDefExt, full, sizeof(full));
        BOOL isDir = FALSE;
        BOOL exists = mywin_path_exists(full, &isDir);
        if ((st->lpofn->Flags & OFN_FILEMUSTEXIST) && (!exists || isDir)) {
            mywin_commdlg_set_status(hDlg, "Multi-select file does not exist");
            return TRUE;
        }
    }

    LPSTR out = st->lpofn->lpstrFile;
    size_t remain = st->lpofn->nMaxFile;
    int n = snprintf(out, remain, "%s", st->currentDir);
    if (n < 0 || (size_t)n >= remain) return TRUE;
    out += n + 1; remain -= (size_t)n + 1;
    for (int i = 0; i < ntok; ++i) {
        char fullName[MAX_PATH];
        mywin_path_join(fullName, sizeof(fullName), st->currentDir, toks[i]);
        mywin_commdlg_append_default_extension_ex(st->lpofn, st->runtimeDefExt, fullName, sizeof(fullName));
        LPCSTR base = mywin_path_basename_local(fullName);
        n = snprintf(out, remain, "%s", base);
        if (n < 0 || (size_t)n >= remain) return TRUE;
        out += n + 1; remain -= (size_t)n + 1;
    }
    if (remain > 0) *out = 0;
    st->lpofn->nFilterIndex = (DWORD)st->currentFilter + 1;
    st->lpofn->nFileOffset = (WORD)(strlen(st->lpofn->lpstrFile) + 1);
    LPCSTR dot = strrchr(st->lpofn->lpstrFile + st->lpofn->nFileOffset, '.');
    st->lpofn->nFileExtension = dot ? (WORD)((dot + 1) - st->lpofn->lpstrFile) : 0;
    if (st->lpofn->lpstrFileTitle && st->lpofn->nMaxFileTitle > 0) {
        char firstFull[MAX_PATH];
        mywin_path_join(firstFull, sizeof(firstFull), st->currentDir, toks[0]);
        mywin_commdlg_append_default_extension_ex(st->lpofn, st->runtimeDefExt, firstFull, sizeof(firstFull));
        snprintf(st->lpofn->lpstrFileTitle, st->lpofn->nMaxFileTitle, "%s", mywin_path_basename_local(firstFull));
    }
    mywin_commdlg_set_error(0);
    EndDialog(hDlg, IDOK);
    return TRUE;
}

static BOOL mywin_commdlg_chdir(HWND hDlg, MyCommDlgFileDialogStateA* st, LPCSTR dirName)
{
    char next[MAX_PATH];
    mywin_path_join(next, sizeof(next), st->currentDir, dirName);
    BOOL isDir = FALSE;
    if (!mywin_path_exists(next, &isDir) || !isDir) {
        mywin_commdlg_set_status(hDlg, "Selected item is not a directory");
        return FALSE;
    }
    snprintf(st->currentDir, sizeof(st->currentDir), "%s", next);
    mywin_path_normalize_slashes(st->currentDir);
    st->pendingOverwrite[0] = 0;
    SetDlgItemTextA(hDlg, MYWIN_CDLG_ID_FILENAME, "");
    mywin_commdlg_fill_file_list(hDlg, st);
    mywin_commdlg_file_notify(st, hDlg, CDN_FOLDERCHANGE, st->currentDir);
    return TRUE;
}

static void mywin_commdlg_update_selected_file(HWND hDlg, MyCommDlgFileDialogStateA* st)
{
    int sel = (int)SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_LIST, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= st->listCount) return;
    if (st->listIsDir[sel]) SetDlgItemTextA(hDlg, MYWIN_CDLG_ID_FILENAME, st->listNames[sel]);
    else SetDlgItemTextA(hDlg, MYWIN_CDLG_ID_FILENAME, st->listNames[sel]);
    char full[MAX_PATH];
    mywin_commdlg_get_current_full_path(hDlg, st, full, sizeof(full));
    mywin_commdlg_file_notify(st, hDlg, CDN_SELCHANGE, full);
}

static void mywin_commdlg_finish_offsets(LPOPENFILENAMEA ofn)
{
    if (!ofn || !ofn->lpstrFile) return;
    LPCSTR base = mywin_path_basename_local(ofn->lpstrFile);
    ofn->nFileOffset = (WORD)(base - ofn->lpstrFile);
    LPCSTR dot = strrchr(base, '.');
    ofn->nFileExtension = dot ? (WORD)((dot + 1) - ofn->lpstrFile) : 0;
    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle > 0) {
        snprintf(ofn->lpstrFileTitle, ofn->nMaxFileTitle, "%s", base);
    }
}

static BOOL mywin_commdlg_try_ok(HWND hDlg, MyCommDlgFileDialogStateA* st)
{
    char name[MAX_PATH];
    name[0] = 0;
    GetDlgItemTextA(hDlg, MYWIN_CDLG_ID_FILENAME, name, sizeof(name));
    if (!name[0]) {
        int sel = (int)SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_LIST, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < st->listCount) snprintf(name, sizeof(name), "%s", st->listNames[sel]);
    }
    if (!name[0]) { mywin_commdlg_set_status(hDlg, "Enter or select a file name"); return FALSE; }
    if (mywin_commdlg_try_multiselect(hDlg, st, name)) return TRUE;

    int sel = (int)SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_LIST, LB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < st->listCount && st->listIsDir[sel] && strcmp(name, st->listNames[sel]) == 0) {
        return mywin_commdlg_chdir(hDlg, st, st->listNames[sel]);
    }

    char full[MAX_PATH];
    mywin_path_join(full, sizeof(full), st->currentDir, name);
    mywin_commdlg_append_default_extension_ex(st->lpofn, st->runtimeDefExt, full, sizeof(full));
    mywin_commdlg_update_extension_flag(st->lpofn, full);

    BOOL isDir = FALSE;
    BOOL exists = mywin_path_exists(full, &isDir);
    if (exists && isDir) return mywin_commdlg_chdir(hDlg, st, name);

    char parent[MAX_PATH];
    mywin_path_dirname(full, parent, sizeof(parent));
    BOOL parentIsDir = FALSE;
    BOOL parentExists = mywin_path_exists(parent, &parentIsDir) && parentIsDir;
    if ((st->lpofn->Flags & OFN_PATHMUSTEXIST) && !parentExists) {
        mywin_commdlg_set_status(hDlg, "Path does not exist (OFN_PATHMUSTEXIST)");
        return FALSE;
    }
    if (!st->saveDialog && (st->lpofn->Flags & OFN_FILEMUSTEXIST) && !exists) {
        mywin_commdlg_set_status(hDlg, "File does not exist (OFN_FILEMUSTEXIST)");
        return FALSE;
    }
    if (st->saveDialog && (st->lpofn->Flags & OFN_OVERWRITEPROMPT) && exists) {
        if (strcmp(st->pendingOverwrite, full) != 0) {
            snprintf(st->pendingOverwrite, sizeof(st->pendingOverwrite), "%s", full);
            mywin_commdlg_set_status(hDlg, "File exists; press Save again to overwrite");
            return FALSE;
        }
    }

    if (mywin_commdlg_file_notify(st, hDlg, CDN_FILEOK, full)) {
        mywin_commdlg_set_status(hDlg, "CDN_FILEOK vetoed by hook");
        return FALSE;
    }

    size_t need = strlen(full) + 1;
    if (!st->lpofn->lpstrFile || st->lpofn->nMaxFile == 0 || need > st->lpofn->nMaxFile) {
        mywin_commdlg_mark_buffer_too_small(st->lpofn, need);
        mywin_commdlg_set_error(FNERR_BUFFERTOOSMALL);
        mywin_commdlg_set_status(hDlg, "lpstrFile buffer too small");
        return FALSE;
    }

    LPCSTR baseForTitle = mywin_path_basename_local(full);
    if (st->lpofn->lpstrFileTitle && st->lpofn->nMaxFileTitle > 0 && strlen(baseForTitle) + 1 > st->lpofn->nMaxFileTitle) {
        mywin_commdlg_mark_buffer_too_small(st->lpofn, need);
        mywin_commdlg_set_error(FNERR_BUFFERTOOSMALL);
        mywin_commdlg_set_status(hDlg, "lpstrFileTitle buffer too small");
        return FALSE;
    }

    snprintf(st->lpofn->lpstrFile, st->lpofn->nMaxFile, "%s", full);
    st->lpofn->nFilterIndex = (DWORD)st->currentFilter + 1;
    if (IsDlgButtonChecked(hDlg, MYWIN_CDLG_ID_READONLY) == BST_CHECKED) st->lpofn->Flags |= OFN_READONLY;
    else st->lpofn->Flags &= ~OFN_READONLY;
    mywin_commdlg_finish_offsets(st->lpofn);
    if (st->lpofn->Flags & OFN_NOCHANGEDIR) SetCurrentDirectoryA(st->oldDir);
    else SetCurrentDirectoryA(parent);
    mywin_commdlg_set_error(0);
    EndDialog(hDlg, IDOK);
    return TRUE;
}

static INT_PTR mywin_commdlg_file_call_hook(MyCommDlgFileDialogStateA* st, HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (!st || !st->lpofn || !(st->lpofn->Flags & OFN_ENABLEHOOK) || !st->lpofn->lpfnHook) return 0;
    LPARAM hookParam = (uMsg == WM_INITDIALOG) ? (LPARAM)st->lpofn : lParam;
    return (INT_PTR)st->lpofn->lpfnHook(hDlg, uMsg, wParam, hookParam);
}

static INT_PTR CALLBACK mywin_commdlg_file_dlgproc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MyCommDlgFileDialogStateA* st = (MyCommDlgFileDialogStateA*)GetWindowLongPtrA(hDlg, GWLP_USERDATA);
    switch (uMsg) {
    case WM_INITDIALOG:
        st = (MyCommDlgFileDialogStateA*)lParam;
        SetWindowLongPtrA(hDlg, GWLP_USERDATA, (LONG_PTR)st);
        if (!st || !st->lpofn) return FALSE;
        if (mywin_commdlg_file_call_hook(st, hDlg, uMsg, wParam, lParam)) return TRUE;
        SetWindowTextA(hDlg, st->lpofn->lpstrTitle ? st->lpofn->lpstrTitle : (st->saveDialog ? "Save As" : "Open"));
        SetDlgItemTextA(hDlg, IDOK, st->saveDialog ? "&Save" : "&Open");
        SetDlgItemTextA(hDlg, MYWIN_CDLG_ID_FILENAME, st->lpofn->lpstrFile ? mywin_path_basename_local(st->lpofn->lpstrFile) : "");
        if (st->lpofn->Flags & OFN_HIDEREADONLY) ShowWindow(GetDlgItem(hDlg, MYWIN_CDLG_ID_READONLY), SW_HIDE);
        if (st->lpofn->Flags & OFN_READONLY) CheckDlgButton(hDlg, MYWIN_CDLG_ID_READONLY, BST_CHECKED);
        mywin_commdlg_fill_filters(hDlg, st);
        mywin_commdlg_fill_file_list(hDlg, st);
        SendMessageA(hDlg, DM_SETDEFID, IDOK, 0);
        SetFocus(GetDlgItem(hDlg, MYWIN_CDLG_ID_FILENAME));
        mywin_commdlg_file_notify(st, hDlg, CDN_INITDONE, NULL);
        return FALSE;
    case CDM_GETSPEC: {
        if (!st) return 0;
        char spec[MAX_PATH];
        mywin_commdlg_get_spec(hDlg, st, spec, sizeof(spec));
        st->cdmQueries++;
        return (INT_PTR)mywin_commdlg_copy_ansi((LPSTR)lParam, (size_t)wParam, spec);
    }
    case CDM_GETFILEPATH: {
        if (!st) return 0;
        char full[MAX_PATH];
        mywin_commdlg_get_current_full_path(hDlg, st, full, sizeof(full));
        st->cdmQueries++;
        return (INT_PTR)mywin_commdlg_copy_ansi((LPSTR)lParam, (size_t)wParam, full);
    }
    case CDM_GETFOLDERPATH:
        if (!st) return 0;
        st->cdmQueries++;
        return (INT_PTR)mywin_commdlg_copy_ansi((LPSTR)lParam, (size_t)wParam, st->currentDir);
    case CDM_SETCONTROLTEXT:
        SetDlgItemTextA(hDlg, (int)wParam, (LPCSTR)lParam);
        return TRUE;
    case CDM_HIDECONTROL:
        ShowWindow(GetDlgItem(hDlg, (int)wParam), SW_HIDE);
        return TRUE;
    case CDM_SETDEFEXT:
        if (st && lParam) snprintf(st->runtimeDefExt, sizeof(st->runtimeDefExt), "%s", (LPCSTR)lParam);
        return TRUE;
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        UINT code = HIWORD(wParam);
        if (id == IDCANCEL) { mywin_commdlg_set_error(0); EndDialog(hDlg, IDCANCEL); return TRUE; }
        if (id == IDOK) { if (st) mywin_commdlg_try_ok(hDlg, st); return TRUE; }
        if (!st) return FALSE;
        if (id == MYWIN_CDLG_ID_LIST && code == LBN_SELCHANGE) { mywin_commdlg_update_selected_file(hDlg, st); return TRUE; }
        if (id == MYWIN_CDLG_ID_FILTER && code == CBN_SELCHANGE) {
            int sel = (int)SendDlgItemMessageA(hDlg, MYWIN_CDLG_ID_FILTER, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < st->filterCount) {
                st->currentFilter = sel;
                st->pendingOverwrite[0] = 0;
                mywin_commdlg_fill_file_list(hDlg, st);
                mywin_commdlg_file_notify(st, hDlg, CDN_TYPECHANGE, st->filters[st->currentFilter].pattern);
            }
            return TRUE;
        }
        break;
    }
    default:
        if (mywin_commdlg_file_call_hook(st, hDlg, uMsg, wParam, lParam)) return TRUE;
        break;
    }
    return FALSE;
}

static BYTE g_MyCommDlgFileTemplateBlob[4096] __attribute__((aligned(4)));
static int  g_MyCommDlgFileTemplateReady = 0;

static void mywin_cdlg_blob_align4(BYTE** pp)
{
    uintptr_t v = (uintptr_t)(*pp);
    v = (v + 3u) & ~(uintptr_t)3u;
    *pp = (BYTE*)v;
}

static void mywin_cdlg_blob_w16(BYTE** pp, WORD v) { memcpy(*pp, &v, sizeof(v)); *pp += sizeof(v); }
static void mywin_cdlg_blob_w32(BYTE** pp, DWORD v) { memcpy(*pp, &v, sizeof(v)); *pp += sizeof(v); }

static void mywin_cdlg_blob_wstr(BYTE** pp, LPCSTR s)
{
    if (!s) s = "";
    while (*s) mywin_cdlg_blob_w16(pp, (WORD)(unsigned char)*s++);
    mywin_cdlg_blob_w16(pp, 0);
}

static void mywin_cdlg_blob_ord(BYTE** pp, WORD atom)
{
    mywin_cdlg_blob_w16(pp, 0xFFFFu);
    mywin_cdlg_blob_w16(pp, atom);
}

static void mywin_cdlg_blob_item(BYTE** pp, DWORD style, DWORD exStyle, short x, short y, short cx, short cy, WORD id, WORD clsAtom, LPCSTR title)
{
    mywin_cdlg_blob_align4(pp);
    mywin_cdlg_blob_w32(pp, style);
    mywin_cdlg_blob_w32(pp, exStyle);
    mywin_cdlg_blob_w16(pp, (WORD)x); mywin_cdlg_blob_w16(pp, (WORD)y);
    mywin_cdlg_blob_w16(pp, (WORD)cx); mywin_cdlg_blob_w16(pp, (WORD)cy);
    mywin_cdlg_blob_w16(pp, id);
    mywin_cdlg_blob_ord(pp, clsAtom);
    mywin_cdlg_blob_wstr(pp, title);
    mywin_cdlg_blob_w16(pp, 0);
}

static LPCDLGTEMPLATEA mywin_commdlg_file_template(void)
{
    if (g_MyCommDlgFileTemplateReady) return (LPCDLGTEMPLATEA)g_MyCommDlgFileTemplateBlob;
    memset(g_MyCommDlgFileTemplateBlob, 0, sizeof(g_MyCommDlgFileTemplateBlob));
    BYTE* p = g_MyCommDlgFileTemplateBlob;
    mywin_cdlg_blob_w32(&p, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    mywin_cdlg_blob_w32(&p, 0);
    mywin_cdlg_blob_w16(&p, 11);
    mywin_cdlg_blob_w16(&p, 0); mywin_cdlg_blob_w16(&p, 0);
    mywin_cdlg_blob_w16(&p, 330); mywin_cdlg_blob_w16(&p, 194);
    mywin_cdlg_blob_w16(&p, 0);  /* menu */
    mywin_cdlg_blob_w16(&p, 0);  /* default #32770 */
    mywin_cdlg_blob_wstr(&p, "Open");
    mywin_cdlg_blob_w16(&p, 8);
    mywin_cdlg_blob_wstr(&p, "MS Shell Dlg");

    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 8, 8, 44, 10, 0x3F10u, MYOS_DLG_CLASS_STATIC, "Look &in:");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, 0, 54, 6, 262, 14, MYWIN_CDLG_ID_PATH, MYOS_DLG_CLASS_EDIT, "");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0, 8, 25, 308, 82, MYWIN_CDLG_ID_LIST, MYOS_DLG_CLASS_LISTBOX, "");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 8, 116, 55, 10, 0x3F11u, MYOS_DLG_CLASS_STATIC, "File &name:");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0, 72, 113, 166, 14, MYWIN_CDLG_ID_FILENAME, MYOS_DLG_CLASS_EDIT, "");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 258, 112, 58, 16, IDOK, MYOS_DLG_CLASS_BUTTON, "&Open");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 8, 137, 76, 10, 0x3F12u, MYOS_DLG_CLASS_STATIC, "Files of &type:");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_BORDER, 0, 90, 134, 148, 14, MYWIN_CDLG_ID_FILTER, MYOS_DLG_CLASS_COMBOBOX, "");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 8, 162, 74, 12, MYWIN_CDLG_ID_READONLY, MYOS_DLG_CLASS_BUTTON, "&Read only");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 258, 134, 58, 16, IDCANCEL, MYOS_DLG_CLASS_BUTTON, "Cancel");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 8, 178, 308, 10, MYWIN_CDLG_ID_STATUS, MYOS_DLG_CLASS_STATIC, "");

    g_MyCommDlgFileTemplateReady = 1;
    return (LPCDLGTEMPLATEA)g_MyCommDlgFileTemplateBlob;
}

static BOOL mywin_commdlg_file_common(LPOPENFILENAMEA lpofn, BOOL saveDialog)
{
    g_CommDlgExtendedError = 0;
    if (!lpofn) { mywin_commdlg_set_error(CDERR_INITIALIZATION); return FALSE; }
    if (lpofn->lStructSize < sizeof(OPENFILENAMEA)) { mywin_commdlg_set_error(CDERR_STRUCTSIZE); return FALSE; }
    if (!lpofn->lpstrFile || lpofn->nMaxFile == 0) { mywin_commdlg_set_error(FNERR_BUFFERTOOSMALL); return FALSE; }
    if (!mywin_commdlg_file_supported_or_error(lpofn)) return FALSE;

    MyCommDlgFileDialogStateA st;
    memset(&st, 0, sizeof(st));
    st.lpofn = lpofn;
    st.saveDialog = saveDialog ? TRUE : FALSE;
    GetCurrentDirectoryA(sizeof(st.oldDir), st.oldDir);

    if (lpofn->lpstrInitialDir && lpofn->lpstrInitialDir[0]) snprintf(st.currentDir, sizeof(st.currentDir), "%s", lpofn->lpstrInitialDir);
    else if (lpofn->lpstrFile && lpofn->lpstrFile[0]) mywin_path_dirname(lpofn->lpstrFile, st.currentDir, sizeof(st.currentDir));
    else snprintf(st.currentDir, sizeof(st.currentDir), "%s", st.oldDir[0] ? st.oldDir : ".");
    mywin_path_normalize_slashes(st.currentDir);
    BOOL isDir = FALSE;
    if (!mywin_path_exists(st.currentDir, &isDir) || !isDir) snprintf(st.currentDir, sizeof(st.currentDir), ".");
    mywin_commdlg_parse_filters(&st);

    HWND owner = lpofn->hwndOwner ? lpofn->hwndOwner : GetForegroundWindow();
    if (!owner) owner = GetDesktopWindow();
    if (!owner) { mywin_commdlg_set_error(CDERR_DIALOGFAILURE); return FALSE; }

    MyCommDlgResolvedTemplateA tpl;
    if (!mywin_commdlg_select_template(lpofn->hInstance, lpofn->lpTemplateName, lpofn->Flags,
                                       OFN_ENABLETEMPLATE, OFN_ENABLETEMPLATEHANDLE,
                                       mywin_commdlg_file_template(), &tpl)) return FALSE;
    INT_PTR r = DialogBoxIndirectParamA(lpofn->hInstance, tpl.lpTemplate, owner, mywin_commdlg_file_dlgproc, (LPARAM)&st);
    mywin_commdlg_release_template(&tpl);
    if (r == IDOK) return TRUE;
    if (r == -1 && g_CommDlgExtendedError == 0) mywin_commdlg_set_error(CDERR_DIALOGFAILURE);
    if (lpofn->Flags & OFN_NOCHANGEDIR) SetCurrentDirectoryA(st.oldDir);
    return FALSE;
}

BOOL GetOpenFileNameA(LPOPENFILENAMEA lpofn)
{
    return mywin_commdlg_file_common(lpofn, FALSE);
}

BOOL GetSaveFileNameA(LPOPENFILENAMEA lpofn)
{
    return mywin_commdlg_file_common(lpofn, TRUE);
}


/* ──────────────────────────────────────────────────────────────────────
   v103 COMDLG32: ChooseFontA

   The dialog intentionally lives on the same USER32/dialog/control surface as
   GetOpenFileNameA: DLGTEMPLATE, #32770, LISTBOX, COMBOBOX, BUTTON, STATIC,
   WM_COMMAND, IsDialogMessageA and EndDialog.
   ────────────────────────────────────────────────────────────────────── */

#define MYWIN_CFONT_ID_FONTLIST   0x3EA0u
#define MYWIN_CFONT_ID_STYLELIST  0x3EA1u
#define MYWIN_CFONT_ID_SIZELIST   0x3EA2u
#define MYWIN_CFONT_ID_STRIKEOUT  0x3EA3u
#define MYWIN_CFONT_ID_UNDERLINE  0x3EA4u
#define MYWIN_CFONT_ID_COLOR      0x3EA5u
#define MYWIN_CFONT_ID_SAMPLE     0x3EA6u
#define MYWIN_CFONT_ID_STATUS     0x3EA7u

#define MYWIN_CFONT_MAX_FACES 16
#define MYWIN_CFONT_MAX_SIZES 16

typedef struct MyCommDlgFontFaceA {
    LPCSTR face;
    BYTE   pitchFamily;
    BOOL   fixedPitch;
} MyCommDlgFontFaceA;

typedef struct MyCommDlgColorChoiceA {
    LPCSTR name;
    COLORREF color;
} MyCommDlgColorChoiceA;

typedef struct MyCommDlgFontDialogStateA {
    LPCHOOSEFONTA lpcf;
    LOGFONTA originalLogFont;
    char originalStyle[32];
    const MyCommDlgFontFaceA* faces[MYWIN_CFONT_MAX_FACES];
    int faceCount;
    int sizeValues[MYWIN_CFONT_MAX_SIZES];
    int sizeCount;
    int initialFace;
    int initialStyle;
    int initialSize;
    int initialColor;
} MyCommDlgFontDialogStateA;

static const MyCommDlgFontFaceA g_MyCommDlgFontFacesA[] = {
    { "MS Shell Dlg",    VARIABLE_PITCH | FF_SWISS,  FALSE },
    { "System",          VARIABLE_PITCH | FF_SWISS,  FALSE },
    { "Fixedsys",        FIXED_PITCH    | FF_MODERN, TRUE  },
    { "Terminal",        FIXED_PITCH    | FF_MODERN, TRUE  },
    { "Courier New",     FIXED_PITCH    | FF_MODERN, TRUE  },
    { "Arial",           VARIABLE_PITCH | FF_SWISS,  FALSE },
    { "Times New Roman", VARIABLE_PITCH | FF_ROMAN,  FALSE },
    { "Segoe UI",        VARIABLE_PITCH | FF_SWISS,  FALSE }
};

static const LPCSTR g_MyCommDlgFontStylesA[] = {
    "Regular", "Bold", "Italic", "Bold Italic"
};

static const int g_MyCommDlgFontSizesA[] = {
    8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72
};

static const MyCommDlgColorChoiceA g_MyCommDlgFontColorsA[] = {
    { "Black",   RGB(0,0,0) },
    { "Gray",    RGB(128,128,128) },
    { "Red",     RGB(220,40,40) },
    { "Green",   RGB(40,180,80) },
    { "Blue",    RGB(80,120,255) },
    { "Cyan",    RGB(0,200,220) },
    { "Magenta", RGB(220,80,220) },
    { "Yellow",  RGB(240,220,60) },
    { "White",   RGB(255,255,255) }
};

static int mywin_abs_int(int v) { return v < 0 ? -v : v; }

static int mywin_choosefont_style_from_logfont(const LOGFONTA* lf, LPCSTR styleText)
{
    if (styleText && styleText[0]) {
        if (strcasecmp(styleText, "Bold Italic") == 0) return 3;
        if (strcasecmp(styleText, "Bold") == 0) return 1;
        if (strcasecmp(styleText, "Italic") == 0) return 2;
        if (strcasecmp(styleText, "Regular") == 0) return 0;
    }
    BOOL bold = lf && lf->lfWeight >= FW_BOLD;
    BOOL italic = lf && lf->lfItalic;
    return bold ? (italic ? 3 : 1) : (italic ? 2 : 0);
}

static void mywin_choosefont_build_faces(MyCommDlgFontDialogStateA* st)
{
    if (!st || !st->lpcf) return;
    st->faceCount = 0;
    for (size_t i = 0; i < sizeof(g_MyCommDlgFontFacesA)/sizeof(g_MyCommDlgFontFacesA[0]); ++i) {
        const MyCommDlgFontFaceA* f = &g_MyCommDlgFontFacesA[i];
        if ((st->lpcf->Flags & CF_FIXEDPITCHONLY) && !f->fixedPitch) continue;
        if (st->faceCount < MYWIN_CFONT_MAX_FACES) st->faces[st->faceCount++] = f;
    }
}

static void mywin_choosefont_build_sizes(MyCommDlgFontDialogStateA* st)
{
    if (!st || !st->lpcf) return;
    st->sizeCount = 0;
    int minSize = (st->lpcf->Flags & CF_LIMITSIZE) ? st->lpcf->nSizeMin : 0;
    int maxSize = (st->lpcf->Flags & CF_LIMITSIZE) ? st->lpcf->nSizeMax : 10000;
    for (size_t i = 0; i < sizeof(g_MyCommDlgFontSizesA)/sizeof(g_MyCommDlgFontSizesA[0]); ++i) {
        int sz = g_MyCommDlgFontSizesA[i];
        if (sz < minSize || sz > maxSize) continue;
        if (st->sizeCount < MYWIN_CFONT_MAX_SIZES) st->sizeValues[st->sizeCount++] = sz;
    }
}

static int mywin_choosefont_find_face(MyCommDlgFontDialogStateA* st, LPCSTR face)
{
    if (!st || st->faceCount <= 0) return 0;
    if (face && face[0]) {
        for (int i = 0; i < st->faceCount; ++i) {
            if (strcasecmp(st->faces[i]->face, face) == 0) return i;
        }
    }
    return 0;
}

static int mywin_choosefont_find_size(MyCommDlgFontDialogStateA* st, int pointSize)
{
    if (!st || st->sizeCount <= 0) return 0;
    if (pointSize <= 0) pointSize = 10;
    int best = 0;
    int bestDelta = mywin_abs_int(st->sizeValues[0] - pointSize);
    for (int i = 1; i < st->sizeCount; ++i) {
        int d = mywin_abs_int(st->sizeValues[i] - pointSize);
        if (d < bestDelta) { best = i; bestDelta = d; }
    }
    return best;
}

static int mywin_choosefont_find_color(COLORREF color)
{
    for (int i = 0; i < (int)(sizeof(g_MyCommDlgFontColorsA)/sizeof(g_MyCommDlgFontColorsA[0])); ++i) {
        if (g_MyCommDlgFontColorsA[i].color == color) return i;
    }
    return 0;
}

static int mywin_choosefont_current_face_index(HWND hDlg, MyCommDlgFontDialogStateA* st)
{
    int sel = (int)SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_FONTLIST, LB_GETCURSEL, 0, 0);
    if (!st || sel < 0 || sel >= st->faceCount) sel = st ? st->initialFace : 0;
    if (sel < 0) sel = 0;
    return sel;
}

static int mywin_choosefont_current_style_index(HWND hDlg)
{
    int sel = (int)SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_STYLELIST, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= 4) sel = 0;
    return sel;
}

static int mywin_choosefont_current_size_index(HWND hDlg, MyCommDlgFontDialogStateA* st)
{
    int sel = (int)SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_SIZELIST, LB_GETCURSEL, 0, 0);
    if (!st || sel < 0 || sel >= st->sizeCount) sel = st ? st->initialSize : 0;
    if (sel < 0) sel = 0;
    return sel;
}

static int mywin_choosefont_current_color_index(HWND hDlg)
{
    int sel = (int)SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_COLOR, CB_GETCURSEL, 0, 0);
    int n = (int)(sizeof(g_MyCommDlgFontColorsA)/sizeof(g_MyCommDlgFontColorsA[0]));
    if (sel < 0 || sel >= n) sel = 0;
    return sel;
}

static void mywin_choosefont_update_preview(HWND hDlg, MyCommDlgFontDialogStateA* st)
{
    if (!st || st->faceCount <= 0 || st->sizeCount <= 0) return;
    int faceIdx = mywin_choosefont_current_face_index(hDlg, st);
    int styleIdx = mywin_choosefont_current_style_index(hDlg);
    int sizeIdx = mywin_choosefont_current_size_index(hDlg, st);
    int colorIdx = mywin_choosefont_current_color_index(hDlg);
    char sample[192];
    snprintf(sample, sizeof(sample), "AaBbYyZz  %s %s %dpt", st->faces[faceIdx]->face,
             g_MyCommDlgFontStylesA[styleIdx], st->sizeValues[sizeIdx]);
    SetDlgItemTextA(hDlg, MYWIN_CFONT_ID_SAMPLE, sample);
    char status[192];
    snprintf(status, sizeof(status), "%s, %s, %d pt, %s", st->faces[faceIdx]->face,
             g_MyCommDlgFontStylesA[styleIdx], st->sizeValues[sizeIdx],
             g_MyCommDlgFontColorsA[colorIdx].name);
    SetDlgItemTextA(hDlg, MYWIN_CFONT_ID_STATUS, status);
}

static void mywin_choosefont_fill_controls(HWND hDlg, MyCommDlgFontDialogStateA* st)
{
    if (!st || !st->lpcf) return;
    for (int i = 0; i < st->faceCount; ++i)
        SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_FONTLIST, LB_ADDSTRING, 0, (LPARAM)st->faces[i]->face);
    for (int i = 0; i < 4; ++i)
        SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_STYLELIST, LB_ADDSTRING, 0, (LPARAM)g_MyCommDlgFontStylesA[i]);
    char buf[32];
    for (int i = 0; i < st->sizeCount; ++i) {
        snprintf(buf, sizeof(buf), "%d", st->sizeValues[i]);
        SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_SIZELIST, LB_ADDSTRING, 0, (LPARAM)buf);
    }
    int colors = (int)(sizeof(g_MyCommDlgFontColorsA)/sizeof(g_MyCommDlgFontColorsA[0]));
    for (int i = 0; i < colors; ++i)
        SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_COLOR, CB_ADDSTRING, 0, (LPARAM)g_MyCommDlgFontColorsA[i].name);

    SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_FONTLIST, LB_SETCURSEL, (WPARAM)st->initialFace, 0);
    SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_STYLELIST, LB_SETCURSEL, (WPARAM)st->initialStyle, 0);
    SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_SIZELIST, LB_SETCURSEL, (WPARAM)st->initialSize, 0);
    SendDlgItemMessageA(hDlg, MYWIN_CFONT_ID_COLOR, CB_SETCURSEL, (WPARAM)st->initialColor, 0);

    if (!(st->lpcf->Flags & CF_EFFECTS)) {
        EnableWindow(GetDlgItem(hDlg, MYWIN_CFONT_ID_STRIKEOUT), FALSE);
        EnableWindow(GetDlgItem(hDlg, MYWIN_CFONT_ID_UNDERLINE), FALSE);
        EnableWindow(GetDlgItem(hDlg, MYWIN_CFONT_ID_COLOR), FALSE);
    } else if (st->lpcf->lpLogFont) {
        if (st->lpcf->lpLogFont->lfStrikeOut) CheckDlgButton(hDlg, MYWIN_CFONT_ID_STRIKEOUT, BST_CHECKED);
        if (st->lpcf->lpLogFont->lfUnderline) CheckDlgButton(hDlg, MYWIN_CFONT_ID_UNDERLINE, BST_CHECKED);
    }
    mywin_choosefont_update_preview(hDlg, st);
}

static BOOL mywin_choosefont_apply_result(HWND hDlg, MyCommDlgFontDialogStateA* st)
{
    if (!st || !st->lpcf || !st->lpcf->lpLogFont) return FALSE;
    if (st->faceCount <= 0 || st->sizeCount <= 0) { mywin_commdlg_set_error(CFERR_NOFONTS); return FALSE; }

    int faceIdx = mywin_choosefont_current_face_index(hDlg, st);
    int styleIdx = mywin_choosefont_current_style_index(hDlg);
    int sizeIdx = mywin_choosefont_current_size_index(hDlg, st);
    int colorIdx = mywin_choosefont_current_color_index(hDlg);
    int sizePt = st->sizeValues[sizeIdx];
    BOOL bold = (styleIdx == 1 || styleIdx == 3) ? TRUE : FALSE;
    BOOL italic = (styleIdx == 2 || styleIdx == 3) ? TRUE : FALSE;

    LOGFONTA* lf = st->lpcf->lpLogFont;
    LOGFONTA out = st->originalLogFont;

    if (!(st->lpcf->Flags & CF_NOSIZESEL)) {
        out.lfHeight = -sizePt;
        st->lpcf->iPointSize = sizePt * 10;
    }
    if (!(st->lpcf->Flags & CF_NOSTYLESEL)) {
        out.lfWeight = bold ? FW_BOLD : FW_NORMAL;
        out.lfItalic = italic ? TRUE : FALSE;
        if (st->lpcf->lpszStyle) snprintf(st->lpcf->lpszStyle, 32, "%s", g_MyCommDlgFontStylesA[styleIdx]);
    } else if (st->lpcf->lpszStyle && st->originalStyle[0]) {
        snprintf(st->lpcf->lpszStyle, 32, "%s", st->originalStyle);
    }
    if (st->lpcf->Flags & CF_EFFECTS) {
        out.lfUnderline = (BYTE)(IsDlgButtonChecked(hDlg, MYWIN_CFONT_ID_UNDERLINE) == BST_CHECKED ? TRUE : FALSE);
        out.lfStrikeOut = (BYTE)(IsDlgButtonChecked(hDlg, MYWIN_CFONT_ID_STRIKEOUT) == BST_CHECKED ? TRUE : FALSE);
        st->lpcf->rgbColors = g_MyCommDlgFontColorsA[colorIdx].color;
    }
    if (!(st->lpcf->Flags & CF_NOFACESEL)) {
        out.lfPitchAndFamily = st->faces[faceIdx]->pitchFamily;
        snprintf(out.lfFaceName, sizeof(out.lfFaceName), "%s", st->faces[faceIdx]->face);
    }
    out.lfCharSet = out.lfCharSet ? out.lfCharSet : DEFAULT_CHARSET;
    out.lfOutPrecision = out.lfOutPrecision ? out.lfOutPrecision : OUT_DEFAULT_PRECIS;
    out.lfClipPrecision = out.lfClipPrecision ? out.lfClipPrecision : CLIP_DEFAULT_PRECIS;
    out.lfQuality = out.lfQuality ? out.lfQuality : DEFAULT_QUALITY;
    *lf = out;

    st->lpcf->nFontType = SCREEN_FONTTYPE | (bold ? BOLD_FONTTYPE : REGULAR_FONTTYPE) | (italic ? ITALIC_FONTTYPE : 0);

    mywin_commdlg_set_error(0);
    EndDialog(hDlg, IDOK);
    return TRUE;
}

static INT_PTR mywin_choosefont_call_hook(MyCommDlgFontDialogStateA* st, HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (!st || !st->lpcf || !(st->lpcf->Flags & CF_ENABLEHOOK) || !st->lpcf->lpfnHook) return 0;
    LPARAM hookParam = (uMsg == WM_INITDIALOG) ? (LPARAM)st->lpcf : lParam;
    return (INT_PTR)st->lpcf->lpfnHook(hDlg, uMsg, wParam, hookParam);
}

static INT_PTR CALLBACK mywin_choosefont_dlgproc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MyCommDlgFontDialogStateA* st = (MyCommDlgFontDialogStateA*)GetWindowLongPtrA(hDlg, GWLP_USERDATA);
    switch (uMsg) {
    case WM_INITDIALOG:
        st = (MyCommDlgFontDialogStateA*)lParam;
        SetWindowLongPtrA(hDlg, GWLP_USERDATA, (LONG_PTR)st);
        if (!st || !st->lpcf) return FALSE;
        if (mywin_choosefont_call_hook(st, hDlg, uMsg, wParam, lParam)) return TRUE;
        SetWindowTextA(hDlg, "Font");
        mywin_choosefont_fill_controls(hDlg, st);
        SendMessageA(hDlg, DM_SETDEFID, IDOK, 0);
        SetFocus(GetDlgItem(hDlg, MYWIN_CFONT_ID_FONTLIST));
        return FALSE;
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        UINT code = HIWORD(wParam);
        if (id == IDCANCEL) { mywin_commdlg_set_error(0); EndDialog(hDlg, IDCANCEL); return TRUE; }
        if (id == IDOK) { if (st) mywin_choosefont_apply_result(hDlg, st); return TRUE; }
        if (!st) return FALSE;
        if ((id == MYWIN_CFONT_ID_FONTLIST || id == MYWIN_CFONT_ID_STYLELIST || id == MYWIN_CFONT_ID_SIZELIST) && code == LBN_SELCHANGE) {
            mywin_choosefont_update_preview(hDlg, st);
            return TRUE;
        }
        if (id == MYWIN_CFONT_ID_COLOR && code == CBN_SELCHANGE) {
            mywin_choosefont_update_preview(hDlg, st);
            return TRUE;
        }
        if (id == MYWIN_CFONT_ID_STRIKEOUT || id == MYWIN_CFONT_ID_UNDERLINE) {
            mywin_choosefont_update_preview(hDlg, st);
            return TRUE;
        }
        break;
    }
    default:
        if (mywin_choosefont_call_hook(st, hDlg, uMsg, wParam, lParam)) return TRUE;
        break;
    }
    return FALSE;
}

static BYTE g_MyCommDlgFontTemplateBlob[4096] __attribute__((aligned(4)));
static int  g_MyCommDlgFontTemplateReady = 0;

static LPCDLGTEMPLATEA mywin_choosefont_template(void)
{
    if (g_MyCommDlgFontTemplateReady) return (LPCDLGTEMPLATEA)g_MyCommDlgFontTemplateBlob;
    memset(g_MyCommDlgFontTemplateBlob, 0, sizeof(g_MyCommDlgFontTemplateBlob));
    BYTE* p = g_MyCommDlgFontTemplateBlob;
    mywin_cdlg_blob_w32(&p, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    mywin_cdlg_blob_w32(&p, 0);
    mywin_cdlg_blob_w16(&p, 16);
    mywin_cdlg_blob_w16(&p, 0); mywin_cdlg_blob_w16(&p, 0);
    mywin_cdlg_blob_w16(&p, 350); mywin_cdlg_blob_w16(&p, 206);
    mywin_cdlg_blob_w16(&p, 0);
    mywin_cdlg_blob_w16(&p, 0);
    mywin_cdlg_blob_wstr(&p, "Font");
    mywin_cdlg_blob_w16(&p, 8);
    mywin_cdlg_blob_wstr(&p, "MS Shell Dlg");

    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 8, 8, 70, 10, 0x3FA0u, MYOS_DLG_CLASS_STATIC, "&Font:");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 134, 8, 70, 10, 0x3FA1u, MYOS_DLG_CLASS_STATIC, "Font st&yle:");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 218, 8, 40, 10, 0x3FA2u, MYOS_DLG_CLASS_STATIC, "&Size:");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0, 8, 20, 118, 86, MYWIN_CFONT_ID_FONTLIST, MYOS_DLG_CLASS_LISTBOX, "");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LBS_NOTIFY, 0, 134, 20, 74, 58, MYWIN_CFONT_ID_STYLELIST, MYOS_DLG_CLASS_LISTBOX, "");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0, 218, 20, 38, 58, MYWIN_CFONT_ID_SIZELIST, MYOS_DLG_CLASS_LISTBOX, "");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 272, 20, 58, 16, IDOK, MYOS_DLG_CLASS_BUTTON, "OK");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 272, 42, 58, 16, IDCANCEL, MYOS_DLG_CLASS_BUTTON, "Cancel");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 134, 88, 70, 10, 0x3FA3u, MYOS_DLG_CLASS_STATIC, "Effects:");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 134, 102, 74, 12, MYWIN_CFONT_ID_STRIKEOUT, MYOS_DLG_CLASS_BUTTON, "Stri&keout");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 134, 118, 74, 12, MYWIN_CFONT_ID_UNDERLINE, MYOS_DLG_CLASS_BUTTON, "&Underline");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 218, 88, 40, 10, 0x3FA4u, MYOS_DLG_CLASS_STATIC, "&Color:");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_BORDER, 0, 218, 101, 74, 14, MYWIN_CFONT_ID_COLOR, MYOS_DLG_CLASS_COMBOBOX, "");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 8, 118, 50, 10, 0x3FA5u, MYOS_DLG_CLASS_STATIC, "Sample:");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | WS_BORDER, 0, 8, 132, 250, 34, MYWIN_CFONT_ID_SAMPLE, MYOS_DLG_CLASS_STATIC, "AaBbYyZz");
    mywin_cdlg_blob_item(&p, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 8, 178, 322, 10, MYWIN_CFONT_ID_STATUS, MYOS_DLG_CLASS_STATIC, "");

    g_MyCommDlgFontTemplateReady = 1;
    return (LPCDLGTEMPLATEA)g_MyCommDlgFontTemplateBlob;
}

BOOL ChooseFontA(LPCHOOSEFONTA lpcf)
{
    g_CommDlgExtendedError = 0;
    if (!lpcf) { mywin_commdlg_set_error(CDERR_INITIALIZATION); return FALSE; }
    if (lpcf->lStructSize < sizeof(CHOOSEFONTA)) { mywin_commdlg_set_error(CDERR_STRUCTSIZE); return FALSE; }
    if (!lpcf->lpLogFont) { mywin_commdlg_set_error(CDERR_INITIALIZATION); return FALSE; }
    if (!mywin_commdlg_font_supported_or_error(lpcf)) return FALSE;
    if ((lpcf->Flags & CF_LIMITSIZE) && lpcf->nSizeMax < lpcf->nSizeMin) {
        mywin_commdlg_set_error(CFERR_MAXLESSTHANMIN);
        return FALSE;
    }

    MyCommDlgFontDialogStateA st;
    memset(&st, 0, sizeof(st));
    st.lpcf = lpcf;
    st.originalLogFont = *lpcf->lpLogFont;
    if (lpcf->lpszStyle) snprintf(st.originalStyle, sizeof(st.originalStyle), "%s", lpcf->lpszStyle);
    mywin_choosefont_build_faces(&st);
    mywin_choosefont_build_sizes(&st);
    if (st.faceCount <= 0 || st.sizeCount <= 0) { mywin_commdlg_set_error(CFERR_NOFONTS); return FALSE; }

    LOGFONTA* lf = lpcf->lpLogFont;
    LPCSTR styleText = (lpcf->Flags & CF_USESTYLE) ? lpcf->lpszStyle : NULL;
    int initPt = 10;
    if (lpcf->iPointSize > 0) initPt = lpcf->iPointSize / 10;
    else if ((lpcf->Flags & CF_INITTOLOGFONTSTRUCT) && lf->lfHeight) initPt = mywin_abs_int((int)lf->lfHeight);
    if (initPt <= 0) initPt = 10;
    if ((lpcf->Flags & CF_FORCEFONTEXIST) && (lpcf->Flags & CF_INITTOLOGFONTSTRUCT) && lf->lfFaceName[0]) {
        BOOL foundFace = FALSE;
        for (int i = 0; i < st.faceCount; ++i) {
            if (strcasecmp(st.faces[i]->face, lf->lfFaceName) == 0) { foundFace = TRUE; break; }
        }
        if (!foundFace) { mywin_commdlg_set_error(CFERR_NOFONTS); return FALSE; }
    }
    st.initialFace = mywin_choosefont_find_face(&st, (lpcf->Flags & CF_INITTOLOGFONTSTRUCT) ? lf->lfFaceName : NULL);
    st.initialStyle = mywin_choosefont_style_from_logfont((lpcf->Flags & CF_INITTOLOGFONTSTRUCT) ? lf : NULL, styleText);
    if (st.initialStyle < 0 || st.initialStyle > 3) st.initialStyle = 0;
    st.initialSize = mywin_choosefont_find_size(&st, initPt);
    st.initialColor = mywin_choosefont_find_color(lpcf->rgbColors);

    HWND owner = lpcf->hwndOwner ? lpcf->hwndOwner : GetForegroundWindow();
    if (!owner) owner = GetDesktopWindow();
    if (!owner) { mywin_commdlg_set_error(CDERR_DIALOGFAILURE); return FALSE; }

    MyCommDlgResolvedTemplateA tpl;
    if (!mywin_commdlg_select_template(lpcf->hInstance, lpcf->lpTemplateName, lpcf->Flags,
                                       CF_ENABLETEMPLATE, CF_ENABLETEMPLATEHANDLE,
                                       mywin_choosefont_template(), &tpl)) return FALSE;
    INT_PTR r = DialogBoxIndirectParamA(lpcf->hInstance, tpl.lpTemplate, owner, mywin_choosefont_dlgproc, (LPARAM)&st);
    mywin_commdlg_release_template(&tpl);
    if (r == IDOK) return TRUE;
    if (r == -1 && g_CommDlgExtendedError == 0) mywin_commdlg_set_error(CDERR_DIALOGFAILURE);
    return FALSE;
}
