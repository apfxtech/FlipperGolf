#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#include "game/game.hpp"
#include "lib/Arduboy2.h"
#include "lib/ArduboyTones.h"
#include "lib/ArduboyFX.h"

// ---------------- Display ----------------
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define FB_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)

// ---------------- Arduboy sound globals (если твой ArduboyTones использует их) ----------------
FuriMessageQueue* g_arduboy_sound_queue = NULL;
FuriThread* g_arduboy_sound_thread = NULL;
volatile bool g_arduboy_sound_thread_running = false;
volatile bool g_arduboy_audio_enabled = false;
volatile bool g_arduboy_tones_playing = false;
volatile bool g_arduboy_force_high = false;
volatile bool g_arduboy_force_norm = false;
volatile uint8_t g_arduboy_volume_mode = VOLUME_IN_TONE;

// rand_seed используется игрой
uint16_t rand_seed = 1;

// ---------------- Arduboy2 adapter ----------------
static Arduboy2Base a;

// ВАЖНО: именно этот буфер читает GUI callback,
// и в него же пишет игра через глобальный `buf`
static uint8_t g_framebuffer[FB_SIZE];

// ВАЖНО: game.hpp при ARDUINO=1 ожидает extern uint8_t* buf;
uint8_t* buf = g_framebuffer;

// ArduboyTones требует callback (в твоей версии)
static bool (*g_audio_enabled_cb)() = nullptr;
static bool audio_enabled_cb() {
    return a.audio.enabled();
}
ArduboyTones sound(g_audio_enabled_cb);

// ---------------- State ----------------
typedef struct {
    Gui* gui;
    Canvas* canvas;
    FuriMutex* mutex;

    FuriPubSub* input_events;
    FuriPubSubSubscription* input_sub;

    volatile uint8_t input_state;
    volatile bool exit_requested;
} FlipperState;

static FlipperState* g_state = nullptr;

// ---------------- Framebuffer callback ----------------
// ВАЖНО: НЕ лочим mutex внутри callback, иначе можно получить дедлок,
// если canvas_commit вызывает callback синхронно из того же потока.
static void framebuffer_commit_callback(
    uint8_t* data,
    size_t size,
    CanvasOrientation orientation,
    void* context
) {
    (void)orientation;
    FlipperState* st = (FlipperState*)context;
    if(!st || !data) return;
    if(size < FB_SIZE) return;

    // Инверсия как в твоём рабочем старом main
    for(size_t i = 0; i < FB_SIZE; i++) {
        data[i] = (uint8_t)(g_framebuffer[i] ^ 0xFF);
    }
}

// ---------------- Input ----------------
static void input_events_callback(const void* value, void* ctx) {
    (void)ctx;
    if(!value || !g_state) return;

    const InputEvent* e = (const InputEvent*)value;

    // long BACK = выход
    if(e->key == InputKeyBack && e->type == InputTypeLong) {
        g_state->exit_requested = true;
        return;
    }

    // Прокидываем в Arduboy input (он обновляет input_state)
    InputEvent ev = *e;
    Arduboy2Base::FlipperInputCallback(&ev, a.inputContext());
}

// ---------------- Platform API expected by game.hpp ----------------
void save_audio_on_off() {
    a.audio.saveOnOff();
    g_arduboy_audio_enabled = a.audio.enabled();
}

void toggle_audio() {
    if(a.audio.enabled()) {
        a.audio.off();
        sound.noTone();
    } else {
        a.audio.on();
    }
    a.audio.saveOnOff();
    g_arduboy_audio_enabled = a.audio.enabled();
}

bool audio_enabled() {
    return a.audio.enabled();
}

uint16_t time_ms() {
    return (uint16_t)millis();
}

// !!! КРИТИЧНО !!!
// Игра ждёт BTN_* (0x80/0x40/0x20/0x10/0x08/0x04)
// Поэтому маппим Arduboy2 кнопки -> BTN_*
uint8_t poll_btns() {
    a.pollButtons();

    uint8_t out = 0;
    if(a.pressed(UP_BUTTON))    out |= BTN_UP;
    if(a.pressed(DOWN_BUTTON))  out |= BTN_DOWN;
    if(a.pressed(LEFT_BUTTON))  out |= BTN_LEFT;
    if(a.pressed(RIGHT_BUTTON)) out |= BTN_RIGHT;
    if(a.pressed(A_BUTTON))     out |= BTN_B;
    if(a.pressed(B_BUTTON))     out |= BTN_A;

    return out;
}

// ---------------- App entry ----------------
extern "C" int32_t arduboy_app(void* p) {
    UNUSED(p);
    buf = g_framebuffer;

    g_state = (FlipperState*)malloc(sizeof(FlipperState));
    if(!g_state) return -1;
    memset(g_state, 0, sizeof(FlipperState));

    g_state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!g_state->mutex) {
        free(g_state);
        g_state = nullptr;
        return -1;
    }

    (void)FX::begin(0, 0);
    memset(g_framebuffer, 0x00, FB_SIZE);
    g_state->input_state = 0;
    g_state->exit_requested = false;
    g_state->gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_framebuffer_callback(g_state->gui, framebuffer_commit_callback, g_state);
    g_state->canvas = gui_direct_draw_acquire(g_state->gui);
    g_state->input_events = (FuriPubSub*)furi_record_open(RECORD_INPUT_EVENTS);
    g_state->input_sub = furi_pubsub_subscribe(
        g_state->input_events, input_events_callback, nullptr
    );

    a.begin(g_framebuffer, &g_state->input_state, g_state->mutex, &g_state->exit_requested);
    a.audio.begin();

    g_arduboy_audio_enabled = a.audio.enabled();

    // ArduboyTones (у тебя требует callback)
    g_audio_enabled_cb = audio_enabled_cb;
    new (&sound) ArduboyTones(g_audio_enabled_cb);

    // seed RNG
    rand_seed = (uint16_t)((furi_hal_random_get() % 65534u) + 1u);


    game_setup();
    furi_mutex_acquire(g_state->mutex, FuriWaitForever);
    game_loop();
    furi_mutex_release(g_state->mutex);
    if(g_state->canvas) canvas_commit(g_state->canvas);

    // ---- main loop ----
    const uint32_t frame_ms = 33; // ~30 fps
    uint32_t next_tick = furi_get_tick();

    while(!g_state->exit_requested) {
        uint32_t now = furi_get_tick();
        if((int32_t)(now - next_tick) < 0) {
            furi_delay_ms(1);
            continue;
        }
        next_tick += frame_ms;

        furi_mutex_acquire(g_state->mutex, FuriWaitForever);
        game_loop();
        furi_mutex_release(g_state->mutex);

        if(g_state->canvas) canvas_commit(g_state->canvas);
    }

    sound.noTone();

    // cleanup
    if(g_state->input_sub) {
        furi_pubsub_unsubscribe(g_state->input_events, g_state->input_sub);
        g_state->input_sub = nullptr;
    }
    if(g_state->input_events) {
        furi_record_close(RECORD_INPUT_EVENTS);
        g_state->input_events = nullptr;
    }

    if(g_state->gui) {
        gui_direct_draw_release(g_state->gui);
        gui_remove_framebuffer_callback(g_state->gui, framebuffer_commit_callback, g_state);
        furi_record_close(RECORD_GUI);
        g_state->gui = nullptr;
        g_state->canvas = nullptr;
    }

    if(g_state->mutex) {
        furi_mutex_free(g_state->mutex);
        g_state->mutex = nullptr;
    }

    free(g_state);
    g_state = nullptr;

    return 0;
}
