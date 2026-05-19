#include "input_layer.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifndef REL_WHEEL_HI_RES
#define REL_WHEEL_HI_RES 0x0b
#endif
#ifndef REL_HWHEEL_HI_RES
#define REL_HWHEEL_HI_RES 0x0c
#endif

/* v92.3: evdev wheel hardening.
   Older builds treated argv[1] as pure keyboard and argv[2] as pure mouse.
   That misses wheels on combo/VM devices and misses modern high-resolution
   wheels that report REL_WHEEL_HI_RES instead of REL_WHEEL.

   myOS now reads both supplied /dev/input/event* fds as generic evdev devices:
     EV_KEY <  BTN_MOUSE  -> keyboard message
     EV_KEY >= BTN_MOUSE  -> mouse button message
     EV_REL REL_X/Y       -> cursor delta
     EV_REL REL_WHEEL     -> MSG_MOUSE_WHEEL
     EV_REL REL_WHEEL_HI_RES -> accumulated MSG_MOUSE_WHEEL detents

   Debugging: run with MYOS_EVDEV_DEBUG=1 to print wheel events. */

static int evdev_debug_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("MYOS_EVDEV_DEBUG");
        cached = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static const char* evdev_code_name(unsigned short type, unsigned short code)
{
    if (type == EV_REL) {
        switch (code) {
        case REL_X: return "REL_X";
        case REL_Y: return "REL_Y";
        case REL_WHEEL: return "REL_WHEEL";
        case REL_HWHEEL: return "REL_HWHEEL";
        case REL_WHEEL_HI_RES: return "REL_WHEEL_HI_RES";
        case REL_HWHEEL_HI_RES: return "REL_HWHEEL_HI_RES";
        default: return "EV_REL";
        }
    }
    if (type == EV_KEY) {
        switch (code) {
        case BTN_LEFT: return "BTN_LEFT";
        case BTN_RIGHT: return "BTN_RIGHT";
        case BTN_MIDDLE: return "BTN_MIDDLE";
        default: return "EV_KEY";
        }
    }
    return "evdev";
}

static void translate_key(const struct input_event* ev, Message* out)
{
    out->type = (ev->value == 0) ? MSG_KEY_UP : MSG_KEY_DOWN;
    out->val1 = ev->code;
    out->val2 = ev->value;
}

static void translate_btn(const struct input_event* ev, Message* out)
{
    out->type = MSG_MOUSE_BTN;
    out->val1 = ev->code;
    out->val2 = ev->value;
}

static void emit_message(InputLayer* il, const Message* msg)
{
    if (il && il->on_message && msg)
        il->on_message(msg, il->userdata);
}

static int clamp16(intptr_t* v)
{
    if (*v < 0) { *v = 0; return 1; }
    if (*v > 65535) { *v = 65535; return 1; }
    return 0;
}

static void emit_wheel(InputLayer* il, int steps, int fd, const struct input_event* ev)
{
    if (steps == 0) return;
    if (evdev_debug_enabled()) {
        fprintf(stderr,
                "[EVDEV] fd=%d %s value=%d -> MSG_MOUSE_WHEEL steps=%d\n",
                fd, evdev_code_name(ev->type, ev->code), ev->value, steps);
    }
    Message msg = { MSG_MOUSE_WHEEL, steps, 0 };
    emit_message(il, &msg);
}

static void read_evdev(InputLayer* il, int fd)
{
    struct input_event ev;
    int has_move = 0;

    /* Virtueller 0..65535-Cursor.
       REL-Mäuse liefern Deltas, ABS-Geräte liefern Positionen.
       Der Desktop erwartet 0..65535, also normalisieren wir REL hier. */
    static intptr_t last_x = 32768;
    static intptr_t last_y = 32768;

    /* High-res wheels are reported in 120-units per normal detent on Linux.
       Accumulate residual values so trackpads/smooth wheels don't get lost. */
    static int hi_res_v_accum = 0;

    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_REL) {
            if (ev.code == REL_X) {
                last_x += (intptr_t)ev.value * 350;
                has_move = 1;
            } else if (ev.code == REL_Y) {
                last_y += (intptr_t)ev.value * 350;
                has_move = 1;
            } else if (ev.code == REL_WHEEL) {
                emit_wheel(il, ev.value, fd, &ev);
            } else if (ev.code == REL_WHEEL_HI_RES) {
                hi_res_v_accum += ev.value;
                int steps = hi_res_v_accum / 120;
                if (steps) {
                    hi_res_v_accum -= steps * 120;
                    emit_wheel(il, steps, fd, &ev);
                } else if (evdev_debug_enabled()) {
                    fprintf(stderr,
                            "[EVDEV] fd=%d %s value=%d accum=%d/120\n",
                            fd, evdev_code_name(ev.type, ev.code), ev.value, hi_res_v_accum);
                }
            } else if (ev.code == REL_HWHEEL || ev.code == REL_HWHEEL_HI_RES) {
                if (evdev_debug_enabled()) {
                    fprintf(stderr,
                            "[EVDEV] fd=%d %s value=%d ignored-horizontal-wheel\n",
                            fd, evdev_code_name(ev.type, ev.code), ev.value);
                }
            }
        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) {
                last_x = ev.value;
                has_move = 1;
            } else if (ev.code == ABS_Y) {
                last_y = ev.value;
                has_move = 1;
            }
        } else if (ev.type == EV_KEY) {
            Message msg = {0};
            if (ev.code >= BTN_MOUSE) {
                translate_btn(&ev, &msg);
            } else {
                translate_key(&ev, &msg);
            }
            emit_message(il, &msg);
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (has_move) {
                clamp16(&last_x);
                clamp16(&last_y);
                Message msg = { MSG_MOUSE_MOVE, last_x, last_y };
                emit_message(il, &msg);
                has_move = 0;
            }
        }
    }
}

int input_layer_init_many(InputLayer* il, int device_count,
                          const char* const* device_paths,
                          InputCallback cb, void* userdata)
{
    if (!il || !device_paths || device_count <= 0) return -1;

    memset(il, 0, sizeof(*il));
    il->on_message = cb;
    il->userdata   = userdata;
    il->running    = 1;
    il->fd_count   = 0;
    for (int i = 0; i < INPUT_LAYER_MAX_DEVICES; i++) il->fds[i] = -1;

    if (device_count > INPUT_LAYER_MAX_DEVICES) {
        fprintf(stderr, "[EVDEV] too many devices (%d), max=%d\n",
                device_count, INPUT_LAYER_MAX_DEVICES);
        return -1;
    }

    for (int i = 0; i < device_count; i++) {
        const char* path = device_paths[i];
        if (!path || !*path) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            perror(path);
            for (int j = 0; j < il->fd_count; j++) {
                if (il->fds[j] >= 0) close(il->fds[j]);
                il->fds[j] = -1;
            }
            il->fd_count = 0;
            return -1;
        }

        int slot = il->fd_count++;
        il->fds[slot] = fd;
        snprintf(il->paths[slot], sizeof(il->paths[slot]), "%s", path);
    }

    if (il->fd_count <= 0) {
        fprintf(stderr, "[EVDEV] no input devices opened\n");
        return -1;
    }

    if (evdev_debug_enabled()) {
        fprintf(stderr, "[EVDEV] generic multi-device reader active: count=%d\n", il->fd_count);
        for (int i = 0; i < il->fd_count; i++) {
            fprintf(stderr, "[EVDEV]   device[%d]=%s fd=%d\n", i, il->paths[i], il->fds[i]);
        }
    }

    return 0;
}

int input_layer_init(InputLayer* il, const char* kbd_path,
                     const char* mouse_path, InputCallback cb,
                     void* userdata)
{
    const char* paths[2] = { kbd_path, mouse_path };
    return input_layer_init_many(il, 2, paths, cb, userdata);
}

void input_layer_run(InputLayer* il)
{
    if (!il || il->fd_count <= 0) return;

    while (il->running) {
        fd_set fds;
        FD_ZERO(&fds);

        int maxfd = -1;
        for (int i = 0; i < il->fd_count; i++) {
            int fd = il->fds[i];
            if (fd < 0) continue;
            FD_SET(fd, &fds);
            if (fd > maxfd) maxfd = fd;
        }

        if (maxfd < 0) break;

        int ready = select(maxfd + 1, &fds, NULL, NULL, NULL);
        if (ready <= 0) continue;

        for (int i = 0; i < il->fd_count; i++) {
            int fd = il->fds[i];
            if (fd >= 0 && FD_ISSET(fd, &fds)) read_evdev(il, fd);
        }
    }
}

void input_layer_stop(InputLayer* il)
{
    if (il) il->running = 0;
}

void input_layer_destroy(InputLayer* il)
{
    if (!il) return;
    for (int i = 0; i < il->fd_count; i++) {
        if (il->fds[i] >= 0) close(il->fds[i]);
        il->fds[i] = -1;
    }
    il->fd_count = 0;
}
