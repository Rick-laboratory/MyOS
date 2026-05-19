#pragma once
#include "message.h"

// ─────────────────────────────────────────────
//  Input-Layer
//  Liest Linux evdev Events vom Kernel und
//  übersetzt sie 1:1 in unser Message-Format.
//  Weiß nichts vom Compositor oder von Apps.
//  Macht genau eine Sache.
// ─────────────────────────────────────────────

// Callback-Typ: input_layer ruft das auf wenn
// eine Message fertig ist. Der Aufrufer entscheidet
// was damit passiert.
typedef void (*InputCallback)(const Message* msg, void* userdata);

#define INPUT_LAYER_MAX_DEVICES 16

typedef struct {
    int            fds[INPUT_LAYER_MAX_DEVICES];
    char           paths[INPUT_LAYER_MAX_DEVICES][128];
    int            fd_count;
    InputCallback  on_message;  // wer die messages bekommt
    void*          userdata;    // kontext für den callback
    int            running;     // 0 = stop
} InputLayer;

// Lifecycle
int  input_layer_init(InputLayer* il, const char* kbd_path,
                      const char* mouse_path, InputCallback cb,
                      void* userdata);
int  input_layer_init_many(InputLayer* il, int device_count,
                           const char* const* device_paths,
                           InputCallback cb, void* userdata);
void input_layer_run(InputLayer* il);   // blockiert - in eigenem thread
void input_layer_stop(InputLayer* il);
void input_layer_destroy(InputLayer* il);
