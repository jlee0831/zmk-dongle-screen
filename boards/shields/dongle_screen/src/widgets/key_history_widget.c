#include <zephyr/kernel.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include <zmk/display.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>
#include "key_history.h"

/* ── Ring buffer — display-thread-only ──────────────────── */

static kh_entry_t kh_buf[KH_RING_BUFFER_SIZE];
static uint8_t    kh_head = 0;
static uint8_t    kh_cnt  = 0;

static void kh_push(const kh_entry_t *entry) {
    kh_buf[kh_head] = *entry;
    kh_head = (kh_head + 1) % KH_RING_BUFFER_SIZE;
    if (kh_cnt < KH_RING_BUFFER_SIZE) {
        kh_cnt++;
    }
}

static bool kh_get(uint8_t newest_offset, kh_entry_t *out) {
    if (newest_offset >= kh_cnt) return false;
    uint8_t idx = (kh_head - 1 - newest_offset + KH_RING_BUFFER_SIZE * 2)
                  % KH_RING_BUFFER_SIZE;
    *out = kh_buf[idx];
    return true;
}

uint8_t kh_count(void) { return kh_cnt; }

/* ── State ───────────────────────────────────────────────── */

static atomic_t kh_recording = ATOMIC_INIT(1);
static atomic_t kh_active    = ATOMIC_INIT(0);
static uint8_t  kh_scroll    = 0;

void    kh_set_recording(bool en) { atomic_set(&kh_recording, en ? 1 : 0); }
bool    kh_is_recording(void)     { return atomic_get(&kh_recording) != 0; }
void    kh_set_active(bool a)     { atomic_set(&kh_active, a ? 1 : 0); }
bool    kh_is_active(void)        { return atomic_get(&kh_active) != 0; }
void    kh_set_scroll_offset(uint8_t o) { kh_scroll = o; }
uint8_t kh_get_scroll_offset(void)      { return kh_scroll; }

/* ── Screen references ───────────────────────────────────── */

static lv_obj_t *s_normal_screen  = NULL;
static lv_obj_t *s_history_screen = NULL;
static lv_obj_t *s_list_cont      = NULL;

void      kh_set_screens(lv_obj_t *normal, lv_obj_t *history) {
    s_normal_screen  = normal;
    s_history_screen = history;
}
lv_obj_t *kh_get_screen(void)        { return s_history_screen; }
lv_obj_t *kh_get_normal_screen(void) { return s_normal_screen; }

/* ── Layer badge colors ───────────────────────────────────── */

typedef struct { uint32_t bg; uint32_t text; } kh_layer_color_t;

static const kh_layer_color_t kh_layer_colors[] = {
    {0x21262d, 0x8b949e},
    {0x1f2d3d, 0x58a6ff},
    {0x2d1f3d, 0xbc8cff},
    {0x2d1d0d, 0xf0883e},
};

static kh_layer_color_t kh_layer_color(uint8_t layer) {
    if (layer < ARRAY_SIZE(kh_layer_colors)) return kh_layer_colors[layer];
    return kh_layer_colors[0];
}

/* ── Keycode → short string ───────────────────────────────── */

static const char *kh_kc_str(uint32_t kc, uint8_t mods, char *buf, size_t len) {
    static const struct { uint32_t kc; const char *s; } tbl[] = {
        {0x04,"A"},{0x05,"B"},{0x06,"C"},{0x07,"D"},{0x08,"E"},
        {0x09,"F"},{0x0A,"G"},{0x0B,"H"},{0x0C,"I"},{0x0D,"J"},
        {0x0E,"K"},{0x0F,"L"},{0x10,"M"},{0x11,"N"},{0x12,"O"},
        {0x13,"P"},{0x14,"Q"},{0x15,"R"},{0x16,"S"},{0x17,"T"},
        {0x18,"U"},{0x19,"V"},{0x1A,"W"},{0x1B,"X"},{0x1C,"Y"},
        {0x1D,"Z"},
        {0x1E,"1"},{0x1F,"2"},{0x20,"3"},{0x21,"4"},{0x22,"5"},
        {0x23,"6"},{0x24,"7"},{0x25,"8"},{0x26,"9"},{0x27,"0"},
        {0x28,"ENT"},{0x29,"ESC"},{0x2A,"BSP"},{0x2B,"TAB"},
        {0x2C,"SPC"},{0x2D,"-"},{0x2E,"="},{0x2F,"["},{0x30,"]"},
        {0x31,"\\"},{0x33,";"},{0x34,"'"},{0x35,"`"},{0x36,","},
        {0x37,"."},{0x38,"/"},{0x39,"CAP"},
        {0x3A,"F1"},{0x3B,"F2"},{0x3C,"F3"},{0x3D,"F4"},
        {0x3E,"F5"},{0x3F,"F6"},{0x40,"F7"},{0x41,"F8"},
        {0x42,"F9"},{0x43,"F10"},{0x44,"F11"},{0x45,"F12"},
        {0x4C,"DEL"},{0x4F,"\xe2\x86\x92"},{0x50,"\xe2\x86\x90"},
        {0x51,"\xe2\x86\x93"},{0x52,"\xe2\x86\x91"},
        {0xE0,"LCT"},{0xE1,"LSH"},{0xE2,"LAL"},{0xE3,"LGU"},
        {0xE4,"RCT"},{0xE5,"RSH"},{0xE6,"RAL"},{0xE7,"RGU"},
    };

    char prefix[10] = "";
    if (mods & 0x02 || mods & 0x20) strncat(prefix, "S+", sizeof(prefix) - strlen(prefix) - 1);
    if (mods & 0x01 || mods & 0x10) strncat(prefix, "C+", sizeof(prefix) - strlen(prefix) - 1);
    if (mods & 0x04 || mods & 0x40) strncat(prefix, "A+", sizeof(prefix) - strlen(prefix) - 1);
    if (mods & 0x08 || mods & 0x80) strncat(prefix, "G+", sizeof(prefix) - strlen(prefix) - 1);

    for (size_t i = 0; i < ARRAY_SIZE(tbl); i++) {
        if (tbl[i].kc == kc) {
            snprintf(buf, len, "%s%s", prefix, tbl[i].s);
            return buf;
        }
    }
    snprintf(buf, len, "%s%02X", prefix, (unsigned)(kc & 0xFF));
    return buf;
}

static const char *kh_layer_name(uint8_t layer) {
    static const char *names[] = {"BASE", "SYM", "NUM", "FNC"};
    if (layer < ARRAY_SIZE(names)) return names[layer];
    static char fallback[5];
    snprintf(fallback, sizeof(fallback), "L%d", layer);
    return fallback;
}

static lv_opa_t kh_age_opacity(uint32_t age_ms) {
    if (age_ms < 2000) return LV_OPA_100;
    if (age_ms < 4000) return LV_OPA_60;
    if (age_ms < 6000) return LV_OPA_30;
    return LV_OPA_10;
}

/* ── Row renderer ────────────────────────────────────────── */

#define KH_SCREEN_W  280
#define KH_HEADER_H   32
#define KH_ROW_H      20

/* One label per row — keeps total LVGL object count within the 10 KB pool */
static void kh_render_row(lv_obj_t *parent, const kh_entry_t *e,
                           uint8_t row_idx, uint32_t age_ms) {
    lv_obj_t *lbl = lv_label_create(parent);
    if (!lbl) return;

    lv_obj_set_pos(lbl, 0, row_idx * KH_ROW_H);
    lv_obj_set_size(lbl, KH_SCREEN_W, KH_ROW_H);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    lv_obj_set_style_pad_left(lbl, 4, 0);
    lv_obj_set_style_pad_top(lbl, 3, 0);
    lv_obj_set_style_border_width(lbl, 0, 0);
    lv_obj_set_style_radius(lbl, 0, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_100, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_opa(lbl, kh_age_opacity(age_ms), 0);

    char buf[64];
    if (e->type == KH_LAYER_CHANGE) {
        lv_obj_set_style_bg_color(lbl, lv_color_hex(0x0d1a0d), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x3fb950), 0);
        snprintf(buf, sizeof(buf), "\xe2\x97\x86 %s %s  %lus",
                 e->layer_active ? "+" : "-",
                 kh_layer_name(e->new_layer),
                 (unsigned long)(age_ms / 1000));
    } else {
        kh_layer_color_t lc = kh_layer_color(e->layer);
        lv_obj_set_style_bg_color(lbl, lv_color_black(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(lc.text), 0);

        char kc_buf[16];
        const char *kc_str = (e->keycode == 0)
            ? "..."
            : kh_kc_str(e->keycode, e->mods, kc_buf, sizeof(kc_buf));
        char pos_str[8];
        if (e->position == 0xFFFFFFFF) {
            snprintf(pos_str, sizeof(pos_str), "??");
        } else {
            snprintf(pos_str, sizeof(pos_str), "%u", (unsigned)e->position);
        }
        snprintf(buf, sizeof(buf), "%-3s \xe2\x86\x92 %-7s [%s] %lus",
                 pos_str, kc_str, kh_layer_name(e->layer),
                 (unsigned long)(age_ms / 1000));
    }
    lv_label_set_text(lbl, buf);
}

/* ── Screen create / rebuild ─────────────────────────────── */

lv_obj_t *kh_screen_create(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, KH_SCREEN_W, 240);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_100, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, KH_SCREEN_W, KH_HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_100, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "KEY HISTORY");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(title, 8, 8);

    lv_obj_t *hint = lv_label_create(header);
    lv_label_set_text(hint, "j/k=scroll  H=close");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x484f58), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -6, 0);

    s_list_cont = lv_obj_create(screen);
    lv_obj_set_size(s_list_cont, KH_SCREEN_W, 240 - KH_HEADER_H);
    lv_obj_set_pos(s_list_cont, 0, KH_HEADER_H);
    lv_obj_set_style_bg_color(s_list_cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_list_cont, LV_OPA_100, 0);
    lv_obj_set_style_border_width(s_list_cont, 0, 0);
    lv_obj_set_style_pad_all(s_list_cont, 0, 0);
    lv_obj_clear_flag(s_list_cont, LV_OBJ_FLAG_SCROLLABLE);

    return screen;
}

void kh_screen_rebuild(void) {
    if (!s_list_cont) return;
    lv_obj_clean(s_list_cont);

    uint8_t count  = kh_count();
    uint8_t offset = kh_get_scroll_offset();

    kh_entry_t newest;
    uint32_t newest_ts = 0;
    if (kh_get(0, &newest)) {
        newest_ts = newest.timestamp_ms;
    }

    uint8_t row  = 0;
    uint8_t skip = offset;
    uint8_t src  = 0;

    while (row < KH_VISIBLE_ROWS && src < count) {
        kh_entry_t e;
        if (!kh_get(src, &e)) break;
        src++;
        if (e.type == KH_KEY_RELEASE) continue;
        if (skip > 0) { skip--; continue; }
        uint32_t age_ms = (newest_ts >= e.timestamp_ms)
                          ? (newest_ts - e.timestamp_ms) : 0;
        kh_render_row(s_list_cont, &e, row, age_ms);
        row++;
    }
}

/* ── ZMK_DISPLAY_WIDGET_LISTENER: position events ───────── */

struct kh_position_state {
    uint32_t position;
    bool     pressed;
    uint8_t  layer;
    uint32_t timestamp_ms;
};

static struct kh_position_state kh_position_get_state(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    return (struct kh_position_state){
        .position     = ev->position,
        .pressed      = ev->state,
        .layer        = zmk_keymap_highest_layer_active(),
        .timestamp_ms = (uint32_t)k_uptime_get(),
    };
}

static void kh_position_update_cb(struct kh_position_state state) {
    if (!kh_is_recording()) return;
    kh_entry_t e = {
        .type         = state.pressed ? KH_KEY_PRESS : KH_KEY_RELEASE,
        .layer        = state.layer,
        .position     = state.position,
        .keycode      = 0,
        .timestamp_ms = state.timestamp_ms,
    };
    kh_push(&e);
    if (kh_is_active()) kh_screen_rebuild();
}

ZMK_DISPLAY_WIDGET_LISTENER(kh_position, struct kh_position_state,
                             kh_position_update_cb, kh_position_get_state)
ZMK_SUBSCRIPTION(kh_position, zmk_position_state_changed);

/* ── ZMK_DISPLAY_WIDGET_LISTENER: layer events ───────────── */

struct kh_layer_state {
    uint8_t  new_layer;
    bool     layer_active;
    uint8_t  highest_layer;
    uint32_t timestamp_ms;
};

static struct kh_layer_state kh_layer_get_state(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    return (struct kh_layer_state){
        .new_layer     = ev->layer,
        .layer_active  = ev->state,
        .highest_layer = zmk_keymap_highest_layer_active(),
        .timestamp_ms  = (uint32_t)k_uptime_get(),
    };
}

static void kh_layer_update_cb(struct kh_layer_state state) {
    if (!kh_is_recording()) return;
    kh_entry_t e = {
        .type         = KH_LAYER_CHANGE,
        .layer        = state.highest_layer,
        .new_layer    = state.new_layer,
        .layer_active = state.layer_active,
        .timestamp_ms = state.timestamp_ms,
    };
    kh_push(&e);
    if (kh_is_active()) kh_screen_rebuild();
}

ZMK_DISPLAY_WIDGET_LISTENER(kh_layer, struct kh_layer_state,
                             kh_layer_update_cb, kh_layer_get_state)
ZMK_SUBSCRIPTION(kh_layer, zmk_layer_state_changed);

/* ── ZMK_DISPLAY_WIDGET_LISTENER: keycode events ────────── */

struct kh_keycode_state {
    uint32_t keycode;
    uint8_t  mods;
    bool     pressed;
    uint8_t  layer;
    uint32_t timestamp_ms;
};

static struct kh_keycode_state kh_keycode_get_state(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    return (struct kh_keycode_state){
        .keycode      = ev->keycode,
        .mods         = ev->explicit_modifiers,
        .pressed      = ev->state,
        .layer        = zmk_keymap_highest_layer_active(),
        .timestamp_ms = (uint32_t)k_uptime_get(),
    };
}

static void kh_keycode_update_cb(struct kh_keycode_state state) {
    if (!kh_is_recording() || !state.pressed) return;

    /* Walk backward for unresolved KH_KEY_PRESS (within last 4 entries) */
    for (uint8_t i = 0; i < kh_cnt && i < 4; i++) {
        uint8_t idx = (kh_head - 1 - i + KH_RING_BUFFER_SIZE * 2) % KH_RING_BUFFER_SIZE;
        if (kh_buf[idx].type == KH_KEY_PRESS && kh_buf[idx].keycode == 0) {
            kh_buf[idx].keycode = state.keycode;
            kh_buf[idx].mods    = state.mods;
            if (kh_is_active()) kh_screen_rebuild();
            return;
        }
    }

    /* No unresolved press — store standalone */
    kh_entry_t e = {
        .type         = KH_KEY_PRESS,
        .layer        = state.layer,
        .position     = 0xFFFFFFFF,
        .keycode      = state.keycode,
        .mods         = state.mods,
        .timestamp_ms = state.timestamp_ms,
    };
    kh_push(&e);
    if (kh_is_active()) kh_screen_rebuild();
}

ZMK_DISPLAY_WIDGET_LISTENER(kh_keycode, struct kh_keycode_state,
                             kh_keycode_update_cb, kh_keycode_get_state)
ZMK_SUBSCRIPTION(kh_keycode, zmk_keycode_state_changed);

/* ── Widget init ─────────────────────────────────────────── */

void kh_widget_init(void) {
    kh_position_init();
    kh_layer_init();
    kh_keycode_init();
}
