#pragma once
#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

#define KH_RING_BUFFER_SIZE 64
#define KH_VISIBLE_ROWS     10  /* (240 - 32) / 20 */

typedef enum {
    KH_KEY_PRESS,
    KH_KEY_RELEASE,
    KH_LAYER_CHANGE,
} kh_event_type_t;

typedef struct {
    kh_event_type_t type;
    uint8_t  layer;        /* active layer at time of event */
    uint32_t position;     /* physical key position (0xFFFFFFFF = unknown) */
    uint32_t keycode;      /* resolved HID keycode (0 = unresolved) */
    uint8_t  mods;         /* explicit modifiers at time of press */
    uint8_t  new_layer;    /* KH_LAYER_CHANGE only */
    bool     layer_active; /* true=activated, false=deactivated (KH_LAYER_CHANGE only) */
    uint32_t timestamp_ms;
    char     bhv[8];       /* short behavior name for non-keycode entries */
} kh_entry_t;

/* Ring buffer — display-thread-only, no mutex */
uint8_t  kh_count(void);

/* State — atomic where cross-thread, display-thread-only otherwise */
void    kh_set_recording(bool enabled);
bool    kh_is_recording(void);
void    kh_set_active(bool active);
bool    kh_is_active(void);
void    kh_set_scroll_offset(uint8_t offset);
uint8_t kh_get_scroll_offset(void);

/* Screen lifecycle — called from display thread */
lv_obj_t *kh_screen_create(void);
void      kh_screen_rebuild(void);
void      kh_set_screens(lv_obj_t *normal, lv_obj_t *history);
lv_obj_t *kh_get_screen(void);
lv_obj_t *kh_get_normal_screen(void);

/* Widget init — registers ZMK_DISPLAY_WIDGET_LISTENER subscriptions */
void kh_widget_init(void);

/* Behavior API — called from ZMK main thread, dispatches to display thread */
void kh_cmd_toggle(void);
void kh_cmd_scroll_up(void);
void kh_cmd_scroll_down(void);
