#include "app_sharedbus.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "window.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): SharedBusLab intentionally combines Sections with signal-only
   IPC. It is useful for cross-process data-plane testing but will break if the
   section namespace, view lifetime or event/signal ownership gets stricter. Use
   it to separate "shared memory contents wrong" from "message signal lost". */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define BUS_NAME "Local\\myos.sharedbus.v24"
#define BUS_MAGIC 0x53425553u /* SBUS */

typedef struct SharedBusPayload {
    DWORD magic;
    DWORD version;
    DWORD producerPid;
    DWORD producerHwnd;
    DWORD bytes;
    char  text[256];
} SharedBusPayload;

typedef struct SharedBusApp {
    HWNDManager* mgr;
    HWND hProducer;
    HWND hConsumer;
    Capability capProducer;
    Capability capConsumer;
    pthread_mutex_t lock;

    HANDLE hMapProducer;
    HANDLE hMapConsumer;
    SharedBusPayload* producerView;
    SharedBusPayload* consumerView;

    DWORD writes;
    DWORD notifies;
    DWORD postedSignals;
    DWORD coalescedSignals;
    DWORD receivedSignals;
    DWORD reads;
    DWORD missed;
    DWORD spamBatches;
    DWORD spam10kBatches;
    DWORD failCount;
    int notifyPending;
    DWORD lastReceivedVersion;
    DWORD lastNotifiedVersion;
    UINT  lastProducerMsg;
    UINT  lastConsumerMsg;
    MyAppResizeState prodResize;
    MyAppResizeState consResize;
    char  lastPayload[256];
    char  prodStatus[160];
    char  consStatus[160];
    char  prodLog[7][112]; int prodLogCount;
    char  consLog[7][112]; int consLogCount;
} SharedBusApp;

static SharedBusApp g_bus;
static int g_bus_initialized = 0;

static void log_push(char log[7][112], int* count, const char* s)
{
    if (!s) return;
    if (*count < 7) snprintf(log[(*count)++], 112, "%s", s);
    else {
        for (int i = 1; i < 7; i++) snprintf(log[i-1], 112, "%s", log[i]);
        snprintf(log[6], 112, "%s", s);
    }
}

static void prod_log_locked(const char* s) { log_push(g_bus.prodLog, &g_bus.prodLogCount, s); }
static void cons_log_locked(const char* s) { log_push(g_bus.consLog, &g_bus.consLogCount, s); }


static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,50,70));
    fb_rect_outline(fb, x, y, w, 20, COLOR(120,140,175));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}

static void bind_prod(void) { MyWinBindRuntime(g_bus.mgr, &g_bus.capProducer); }
static void bind_cons(void) { MyWinBindRuntime(g_bus.mgr, &g_bus.capConsumer); }

static int ensure_producer_map(void)
{
    bind_prod();
    if (!g_bus.hMapProducer) g_bus.hMapProducer = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SharedBusPayload), BUS_NAME);
    if (g_bus.hMapProducer && !g_bus.producerView)
        g_bus.producerView = (SharedBusPayload*)MapViewOfFile(g_bus.hMapProducer, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBusPayload));
    if (g_bus.producerView && g_bus.producerView->magic != BUS_MAGIC) {
        memset(g_bus.producerView, 0, sizeof(SharedBusPayload));
        g_bus.producerView->magic = BUS_MAGIC;
    }
    return g_bus.hMapProducer && g_bus.producerView;
}

static int ensure_consumer_map(void)
{
    bind_cons();
    if (!g_bus.hMapConsumer) g_bus.hMapConsumer = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SharedBusPayload), BUS_NAME);
    if (g_bus.hMapConsumer && !g_bus.consumerView)
        g_bus.consumerView = (SharedBusPayload*)MapViewOfFile(g_bus.hMapConsumer, FILE_MAP_READ, 0, 0, sizeof(SharedBusPayload));
    return g_bus.hMapConsumer && g_bus.consumerView;
}

static void post_prod(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_bus.mgr && g_bus.hProducer) hwnd_post(g_bus.mgr, &g_bus.capProducer, g_bus.hProducer, msg, wp, lp);
}

static void post_cons(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_bus.mgr && g_bus.hConsumer) hwnd_post(g_bus.mgr, &g_bus.capConsumer, g_bus.hConsumer, msg, wp, lp);
}

static void producer_create_bus(void)
{
    pthread_mutex_lock(&g_bus.lock);
    int ok = ensure_producer_map();
    if (ok) {
        snprintf(g_bus.prodStatus, sizeof(g_bus.prodStatus), "CREATE OK section='%s' h=0x%x view=%p", BUS_NAME, g_bus.hMapProducer, (void*)g_bus.producerView);
        prod_log_locked("CREATE: shared bus section mapped for producer");
    } else {
        g_bus.failCount++;
        snprintf(g_bus.prodStatus, sizeof(g_bus.prodStatus), "CREATE failed - CAP_SECTION_MAP?");
        prod_log_locked("CREATE: FAILED");
    }
    pthread_mutex_unlock(&g_bus.lock);
}

static void consumer_map_bus(void)
{
    pthread_mutex_lock(&g_bus.lock);
    int ok = ensure_consumer_map();
    if (ok) {
        snprintf(g_bus.consStatus, sizeof(g_bus.consStatus), "MAP OK section='%s' h=0x%x view=%p", BUS_NAME, g_bus.hMapConsumer, (void*)g_bus.consumerView);
        cons_log_locked("MAP: consumer mapped same named section");
    } else {
        g_bus.failCount++;
        snprintf(g_bus.consStatus, sizeof(g_bus.consStatus), "MAP failed - CAP_SECTION_MAP?");
        cons_log_locked("MAP: FAILED");
    }
    pthread_mutex_unlock(&g_bus.lock);
}

static void consumer_read_locked(const char* reason)
{
    if (!g_bus.consumerView) ensure_consumer_map();
    if (!g_bus.consumerView || g_bus.consumerView->magic != BUS_MAGIC) {
        g_bus.failCount++;
        snprintf(g_bus.consStatus, sizeof(g_bus.consStatus), "%s: no valid producer payload", reason ? reason : "READ");
        cons_log_locked("READ: FAILED no valid payload");
        return;
    }
    DWORD v = g_bus.consumerView->version;
    if (g_bus.lastReceivedVersion && v > g_bus.lastReceivedVersion + 1)
        g_bus.missed += (v - g_bus.lastReceivedVersion - 1);
    g_bus.lastReceivedVersion = v;
    g_bus.reads++;
    snprintf(g_bus.lastPayload, sizeof(g_bus.lastPayload), "%s", g_bus.consumerView->text);
    snprintf(g_bus.consStatus, sizeof(g_bus.consStatus), "%s: v%u '%.96s'", reason ? reason : "READ", v, g_bus.lastPayload);
    char line[112];
    snprintf(line, sizeof(line), "READ: v%u bytes=%u recvSig=%u missed=%u", v, g_bus.consumerView->bytes, g_bus.receivedSignals, g_bus.missed);
    cons_log_locked(line);
}

static void producer_write_notify(int spamIndex)
{
    HWND hConsumer = 0;
    DWORD version = 0;
    int shouldPost = 0;
    int coalesced = 0;

    pthread_mutex_lock(&g_bus.lock);
    if (!ensure_producer_map()) {
        g_bus.failCount++;
        snprintf(g_bus.prodStatus, sizeof(g_bus.prodStatus), "WRITE failed: no producer map");
        prod_log_locked("WRITE: FAILED no producer map");
        pthread_mutex_unlock(&g_bus.lock);
        return;
    }

    g_bus.writes++;
    g_bus.producerView->magic = BUS_MAGIC;
    g_bus.producerView->version = g_bus.writes;
    g_bus.producerView->producerPid = g_bus.capProducer.id;
    g_bus.producerView->producerHwnd = g_bus.hProducer;
    if (spamIndex > 0)
        snprintf(g_bus.producerView->text, sizeof(g_bus.producerView->text), "SharedBus spam #%d / latest version %u from Producer PID=%u HWND=%u", spamIndex, g_bus.writes, g_bus.capProducer.id, g_bus.hProducer);
    else
        snprintf(g_bus.producerView->text, sizeof(g_bus.producerView->text), "SharedBus payload version %u from Producer PID=%u HWND=%u", g_bus.writes, g_bus.capProducer.id, g_bus.hProducer);
    g_bus.producerView->bytes = (DWORD)strlen(g_bus.producerView->text) + 1;
    FlushViewOfFile(g_bus.producerView, sizeof(SharedBusPayload));
    g_bus.lastNotifiedVersion = g_bus.producerView->version;
    g_bus.notifies++;

    version = g_bus.producerView->version;
    hConsumer = g_bus.hConsumer;

    if (!g_bus.notifyPending) {
        g_bus.notifyPending = 1;
        g_bus.postedSignals++;
        shouldPost = 1;
    } else {
        g_bus.coalescedSignals++;
        coalesced = 1;
    }

    if (spamIndex > 0) {
        snprintf(g_bus.prodStatus, sizeof(g_bus.prodStatus), "WRITE v%u dirty-lane -> consumer HWND=%u %s", version, hConsumer, coalesced ? "COALESCED" : "POSTED");
    } else {
        snprintf(g_bus.prodStatus, sizeof(g_bus.prodStatus), "WRITE+NOTIFY v%u -> consumer HWND=%u %s", version, hConsumer, coalesced ? "COALESCED" : "POSTED");
    }

    char line[112];
    if (spamIndex <= 0 || spamIndex <= 3 || (spamIndex % 1000) == 0) {
        snprintf(line, sizeof(line), "WRITE: v%u, %s signal posted=%u coal=%u", version, coalesced ? "coalesced" : "dirty", g_bus.postedSignals, g_bus.coalescedSignals);
        prod_log_locked(line);
    }
    pthread_mutex_unlock(&g_bus.lock);

    if (shouldPost && hConsumer) {
        /* Coalesced event lane: one pending BUSLAB_NOTIFY represents latest section version. */
        hwnd_post(g_bus.mgr, &g_bus.capProducer, hConsumer, BUSLAB_NOTIFY, (WPARAM)version, (LPARAM)g_bus.hMapProducer);
    }
}

static void producer_spam_100(void)
{
    pthread_mutex_lock(&g_bus.lock);
    g_bus.spamBatches++;
    pthread_mutex_unlock(&g_bus.lock);
    for (int i = 1; i <= 100; i++) producer_write_notify(i);
}

static void producer_spam_10k(void)
{
    pthread_mutex_lock(&g_bus.lock);
    g_bus.spam10kBatches++;
    pthread_mutex_unlock(&g_bus.lock);
    for (int i = 1; i <= 10000; i++) producer_write_notify(i);
}

static void consumer_clear(void)
{
    pthread_mutex_lock(&g_bus.lock);
    g_bus.reads = 0;
    g_bus.missed = 0;
    g_bus.receivedSignals = 0;
    g_bus.lastReceivedVersion = 0;
    g_bus.lastPayload[0] = 0;
    g_bus.consLogCount = 0;
    snprintf(g_bus.consStatus, sizeof(g_bus.consStatus), "Consumer counters cleared");
    cons_log_locked("CLEAR: counters reset");
    pthread_mutex_unlock(&g_bus.lock);
}

static void producer_post_command(UINT cmd)
{
    post_prod(WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
}

static void consumer_post_command(UINT cmd)
{
    post_cons(WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
}

static void producer_hit(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8 && cx < 130) { producer_post_command(BUSLAB_CREATE_BUS); return; }
        if (cx >= 138 && cx < 280) { producer_post_command(BUSLAB_WRITE_NOTIFY); return; }
        if (cx >= 288 && cx < 402) { producer_post_command(BUSLAB_SPAM_100); return; }
        if (cx >= 410 && cx < 524) { producer_post_command(BUSLAB_SPAM_10K); return; }
    }
}

static void consumer_hit(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8 && cx < 120) { consumer_post_command(BUSLAB_MAP_BUS); return; }
        if (cx >= 128 && cx < 242) { consumer_post_command(BUSLAB_READ_NOW); return; }
        if (cx >= 250 && cx < 346) { consumer_post_command(BUSLAB_CLEAR); return; }
    }
}

static void producer_handle_command(UINT cmd)
{
    switch (cmd) {
    case BUSLAB_CREATE_BUS: producer_create_bus(); break;
    case BUSLAB_WRITE_NOTIFY: producer_write_notify(0); break;
    case BUSLAB_SPAM_100: producer_spam_100(); break;
    case BUSLAB_SPAM_10K: producer_spam_10k(); break;
    default: break;
    }
}

static void consumer_handle_command(UINT cmd)
{
    switch (cmd) {
    case BUSLAB_MAP_BUS: consumer_map_bus(); break;
    case BUSLAB_READ_NOW:
        pthread_mutex_lock(&g_bus.lock); consumer_read_locked("READ NOW"); pthread_mutex_unlock(&g_bus.lock);
        break;
    case BUSLAB_CLEAR: consumer_clear(); break;
    default: break;
    }
}

static void producer_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, void* userdata)
{
    (void)userdata;
    pthread_mutex_lock(&g_bus.lock); g_bus.lastProducerMsg = msg; pthread_mutex_unlock(&g_bus.lock);
    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_bus.lock);
        g_bus.hProducer = hwnd;
        MyAppResizeInit(&g_bus.prodResize, SHARED_BUS_W, SHARED_BUS_H, TITLEBAR_H);
        snprintf(g_bus.prodStatus, sizeof(g_bus.prodStatus), "Producer ready - create bus, write, notify");
        prod_log_locked("WM_CREATE: producer signal-only IPC sender");
        pthread_mutex_unlock(&g_bus.lock);
        break;
    case WM_LBUTTONDOWN: producer_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); break;
    case WM_COMMAND: producer_handle_command((UINT)LOWORD(wp)); break;
    case BUSLAB_CREATE_BUS: producer_handle_command(BUSLAB_CREATE_BUS); break;
    case BUSLAB_WRITE_NOTIFY: producer_handle_command(BUSLAB_WRITE_NOTIFY); break;
    case BUSLAB_SPAM_100: producer_handle_command(BUSLAB_SPAM_100); break;
    case BUSLAB_SPAM_10K: producer_handle_command(BUSLAB_SPAM_10K); break;
    case WM_GETMINMAXINFO: MyAppResizeOnGetMinMaxInfo(&g_bus.prodResize, lp, SHARED_BUS_MIN_W, SHARED_BUS_MIN_H); break;
    case WM_WINDOWPOSCHANGING: MyAppResizeOnWindowPosChanging(&g_bus.prodResize, lp); break;
    case WM_WINDOWPOSCHANGED: MyAppResizeOnWindowPosChanged(&g_bus.prodResize, lp, TITLEBAR_H); break;
    case WM_MOVE: MyAppResizeOnMove(&g_bus.prodResize, lp); break;
    case WM_SIZE: MyAppResizeOnSize(&g_bus.prodResize, wp, lp); break;
    case WM_CLOSE: DestroyWindow(hwnd); break;
    default: break;
    }
}

static void consumer_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, void* userdata)
{
    (void)userdata;
    pthread_mutex_lock(&g_bus.lock); g_bus.lastConsumerMsg = msg; pthread_mutex_unlock(&g_bus.lock);
    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_bus.lock);
        g_bus.hConsumer = hwnd;
        MyAppResizeInit(&g_bus.consResize, SHARED_BUS_W, SHARED_BUS_H, TITLEBAR_H);
        snprintf(g_bus.consStatus, sizeof(g_bus.consStatus), "Consumer ready - map bus or wait for notify");
        cons_log_locked("WM_CREATE: consumer reads payload from section");
        pthread_mutex_unlock(&g_bus.lock);
        break;
    case WM_LBUTTONDOWN: consumer_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); break;
    case WM_COMMAND: consumer_handle_command((UINT)LOWORD(wp)); break;
    case BUSLAB_MAP_BUS: consumer_handle_command(BUSLAB_MAP_BUS); break;
    case BUSLAB_READ_NOW: consumer_handle_command(BUSLAB_READ_NOW); break;
    case BUSLAB_CLEAR: consumer_handle_command(BUSLAB_CLEAR); break;
    case BUSLAB_NOTIFY:
        pthread_mutex_lock(&g_bus.lock);
        g_bus.receivedSignals++;
        g_bus.notifyPending = 0;
        g_bus.lastNotifiedVersion = (DWORD)wp;
        consumer_read_locked("DIRTY");
        pthread_mutex_unlock(&g_bus.lock);
        break;
    case WM_GETMINMAXINFO: MyAppResizeOnGetMinMaxInfo(&g_bus.consResize, lp, SHARED_BUS_MIN_W, SHARED_BUS_MIN_H); break;
    case WM_WINDOWPOSCHANGING: MyAppResizeOnWindowPosChanging(&g_bus.consResize, lp); break;
    case WM_WINDOWPOSCHANGED: MyAppResizeOnWindowPosChanged(&g_bus.consResize, lp, TITLEBAR_H); break;
    case WM_MOVE: MyAppResizeOnMove(&g_bus.consResize, lp); break;
    case WM_SIZE: MyAppResizeOnSize(&g_bus.consResize, wp, lp); break;
    case WM_CLOSE: DestroyWindow(hwnd); break;
    default: break;
    }
}

static void init_once(HWNDManager* mgr)
{
    if (!g_bus_initialized) {
        memset(&g_bus, 0, sizeof(g_bus));
        pthread_mutex_init(&g_bus.lock, NULL);
        g_bus_initialized = 1;
    }
    g_bus.mgr = mgr;
}

HWND sharedbus_create_producer(HWNDManager* mgr, Capability cap)
{
    init_once(mgr);
    g_bus.capProducer = cap;
    HWND h = hwnd_create(mgr, producer_wndproc, NULL, cap);
    g_bus.hProducer = h;
    return h;
}

HWND sharedbus_create_consumer(HWNDManager* mgr, Capability cap)
{
    init_once(mgr);
    g_bus.capConsumer = cap;
    HWND h = hwnd_create(mgr, consumer_wndproc, NULL, cap);
    g_bus.hConsumer = h;
    return h;
}

void sharedbus_destroy(void)
{
    if (!g_bus_initialized) return;
    bind_prod();
    if (g_bus.producerView) { UnmapViewOfFile(g_bus.producerView); g_bus.producerView = NULL; }
    if (g_bus.consumerView) { UnmapViewOfFile(g_bus.consumerView); g_bus.consumerView = NULL; }
    if (g_bus.hMapProducer) { CloseHandle(g_bus.hMapProducer); g_bus.hMapProducer = 0; }
    if (g_bus.hMapConsumer && g_bus.hMapConsumer != g_bus.hMapProducer) { CloseHandle(g_bus.hMapConsumer); g_bus.hMapConsumer = 0; }
}

static void draw_panel(HWND hwnd, int isProducer, int x, int y, int w, int h, Framebuffer* fb)
{
    int cx = x + 1;
    int cy = y + TITLEBAR_H;
    int cw = w - 2;
    int ch = h - TITLEBAR_H - 1;
    if (cw < 80 || ch < 60) return;

    pthread_mutex_lock(&g_bus.lock);
    DWORD writes = g_bus.writes;
    DWORD notifies = g_bus.notifies;
    DWORD postedSignals = g_bus.postedSignals;
    DWORD coalescedSignals = g_bus.coalescedSignals;
    DWORD receivedSignals = g_bus.receivedSignals;
    DWORD reads = g_bus.reads;
    DWORD missed = g_bus.missed;
    DWORD spam = g_bus.spamBatches;
    DWORD spam10k = g_bus.spam10kBatches;
    DWORD fails = g_bus.failCount;
    int pending = g_bus.notifyPending;
    DWORD lastV = g_bus.lastReceivedVersion;
    DWORD lastN = g_bus.lastNotifiedVersion;
    HANDLE hp = g_bus.hMapProducer;
    HANDLE hc = g_bus.hMapConsumer;
    int mappedP = g_bus.producerView != NULL;
    int mappedC = g_bus.consumerView != NULL;
    char payload[256]; snprintf(payload, sizeof(payload), "%s", g_bus.lastPayload[0] ? g_bus.lastPayload : "<none yet>");
    char status[160]; snprintf(status, sizeof(status), "%s", isProducer ? g_bus.prodStatus : g_bus.consStatus);
    char log[7][112]; int logCount = isProducer ? g_bus.prodLogCount : g_bus.consLogCount;
    memcpy(log, isProducer ? g_bus.prodLog : g_bus.consLog, sizeof(log));
    UINT lastMsg = isProducer ? g_bus.lastProducerMsg : g_bus.lastConsumerMsg;
    pthread_mutex_unlock(&g_bus.lock);

    bind_prod();
    DWORD secCount = MyGetSectionCount();
    DWORD viewCount = MyGetMappedViewCount();

    fb_rect(fb, cx, cy, cw, ch, COLOR(10,12,20));
    fb_rect_outline(fb, cx, cy, cw, ch, COLOR(70,90,130));

    if (isProducer) {
        button(fb, cx + 8,   cy + 8,  122, "Create Bus");
        button(fb, cx + 138, cy + 8,  142, "Write+Notify");
        button(fb, cx + 288, cy + 8,  114, "Spam 100");
        button(fb, cx + 410, cy + 8,  114, "Spam 10k");
    } else {
        button(fb, cx + 8,   cy + 8,  112, "Map Bus");
        button(fb, cx + 128, cy + 8,  114, "Read Now");
        button(fb, cx + 250, cy + 8,  96,  "Clear");
    }

    char line[256];
    if (isProducer) {
        snprintf(line, sizeof(line), "PROD HWND=%u map=0x%x view=%s writes=%u dirty=%u posted=%u coal=%u", hwnd, hp, mappedP?"YES":"no", writes, notifies, postedSignals, coalescedSignals);
        draw_clip_text(fb, cx + 8, cy + 42, line, COLOR(180,255,190), cx + 8, cy + 38, cw - 16, 12);
        snprintf(line, sizeof(line), "sections=%u views=%u consumerHWND=%u spam100=%u spam10k=%u pending=%d msg=0x%04x fails=%u", secCount, viewCount, g_bus.hConsumer, spam, spam10k, pending, lastMsg, fails);
    } else {
        snprintf(line, sizeof(line), "CONS HWND=%u map=0x%x view=%s reads=%u recvSig=%u missed=%u lastV=%u", hwnd, hc, mappedC?"YES":"no", reads, receivedSignals, missed, lastV);
        draw_clip_text(fb, cx + 8, cy + 42, line, COLOR(180,255,190), cx + 8, cy + 38, cw - 16, 12);
        snprintf(line, sizeof(line), "latestDirty=%u msg=0x%04x payload='%.80s'", lastN, lastMsg, payload);
    }
    draw_clip_text(fb, cx + 8, cy + 58, line, COLOR(220,220,240), cx + 8, cy + 54, cw - 16, 12);

    fb_rect(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(18,18,28));
    fb_rect_outline(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(70,90,130));
    int yline = cy + 86;
    for (int i = 0; i < logCount; i++) {
        draw_clip_text(fb, cx + 14, yline, log[i], COLOR(210,210,225), cx + 10, cy + 80, cw - 20, ch - 116);
        yline += 14;
    }
    Color sc = strstr(status, "FAILED") || strstr(status, "failed") ? COLOR(255,165,140) : COLOR(180,255,190);
    draw_clip_text(fb, cx + 14, cy + ch - 24, status, sc, cx + 10, cy + ch - 28, cw - 20, 14);
}

void sharedbus_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    int isProd = hwnd == g_bus.hProducer;
    draw_panel(hwnd, isProd, wx, wy, ww, wh, fb);
}
