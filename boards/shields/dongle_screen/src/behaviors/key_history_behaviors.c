#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <lvgl.h>
#include "key_history.h"

/* ── Display-thread work items ───────────────────────────── */

static void do_toggle(struct k_work *work) {
    bool now_active = !kh_is_active();
    kh_set_active(now_active);
    kh_set_recording(!now_active);

    if (now_active) {
        kh_set_scroll_offset(0);
        kh_screen_rebuild();
        lv_scr_load(kh_get_screen());
    } else {
        lv_scr_load(kh_get_normal_screen());
    }
}
K_WORK_DEFINE(toggle_work, do_toggle);

static void do_scroll_up(struct k_work *work) {
    uint8_t count      = kh_count();
    uint8_t max_offset = (count > KH_VISIBLE_ROWS) ? (count - KH_VISIBLE_ROWS) : 0;
    uint8_t cur        = kh_get_scroll_offset();
    if (cur < max_offset) {
        kh_set_scroll_offset(cur + 1);
        kh_screen_rebuild();
    }
}
K_WORK_DEFINE(scroll_up_work, do_scroll_up);

static void do_scroll_down(struct k_work *work) {
    uint8_t cur = kh_get_scroll_offset();
    if (cur > 0) {
        kh_set_scroll_offset(cur - 1);
        kh_screen_rebuild();
    }
}
K_WORK_DEFINE(scroll_down_work, do_scroll_down);

/* ── Behavior API (called from ZMK main thread) ─────────── */

void kh_cmd_toggle(void) {
    k_work_submit_to_queue(zmk_display_work_q(), &toggle_work);
}
void kh_cmd_scroll_up(void) {
    if (kh_is_active()) {
        k_work_submit_to_queue(zmk_display_work_q(), &scroll_up_work);
    }
}
void kh_cmd_scroll_down(void) {
    if (kh_is_active()) {
        k_work_submit_to_queue(zmk_display_work_q(), &scroll_down_work);
    }
}

/* ── Toggle behavior driver ─────────────────────────────── */

static int kh_toggle_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    kh_cmd_toggle();
    return ZMK_BEHAVIOR_OPAQUE;
}
static int kh_toggle_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}
static const struct behavior_driver_api kh_toggle_driver_api = {
    .binding_pressed  = kh_toggle_pressed,
    .binding_released = kh_toggle_released,
};

#define DT_DRV_COMPAT zmk_key_history_toggle
#define KH_TOGGLE_INST(n) \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, \
        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
        &kh_toggle_driver_api);
DT_INST_FOREACH_STATUS_OKAY(KH_TOGGLE_INST)
#undef DT_DRV_COMPAT

/* ── Scroll behavior driver ─────────────────────────────── */

struct kh_scroll_cfg { int direction; };

static int kh_scroll_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct kh_scroll_cfg *cfg = dev->config;
    if (cfg->direction == 0) {
        kh_cmd_scroll_up();
    } else {
        kh_cmd_scroll_down();
    }
    return ZMK_BEHAVIOR_OPAQUE;
}
static int kh_scroll_released(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}
static const struct behavior_driver_api kh_scroll_driver_api = {
    .binding_pressed  = kh_scroll_pressed,
    .binding_released = kh_scroll_released,
};

#define DT_DRV_COMPAT zmk_key_history_scroll
#define KH_SCROLL_INST(n)                                       \
    static const struct kh_scroll_cfg kh_scroll_cfg_##n = {    \
        .direction = DT_INST_PROP(n, direction),                \
    };                                                           \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL,                \
        &kh_scroll_cfg_##n,                                     \
        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,       \
        &kh_scroll_driver_api);
DT_INST_FOREACH_STATUS_OKAY(KH_SCROLL_INST)
#undef DT_DRV_COMPAT

/* ── Keycode interceptor ─────────────────────────────────── */

/*
 * While the history overlay is open, consume ALL non-modifier keypresses
 * so nothing leaks to the host.  Modifier keycodes (0xE0–0xE7) are let
 * through so that mod-morph behaviors (e.g. ctrl+P → UP arrow) continue
 * to work on the keyboard side.
 *
 * Navigation keycodes also trigger scroll / close actions:
 *   UP  (0x52), k (0x0E)           → scroll up
 *   DOWN(0x51), j (0x0D)           → scroll down
 *   ctrl+p (0x13 + ctrl modifier)  → scroll up   (emacs; also handled by
 *   ctrl+n (0x11 + ctrl modifier)  → scroll down   mod-morph → UP/DOWN)
 *   ESC (0x29)                     → close overlay
 *
 * Track the last consumed keycode so its release is also eaten even after
 * the overlay has closed (e.g. ESC press closes → release still consumed).
 */
static uint32_t s_consumed_kc   = 0;
static uint16_t s_consumed_page = 0;

static int kh_key_nav(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev) return ZMK_EV_EVENT_BUBBLE;

    uint32_t kc   = ev->keycode;
    uint16_t page = ev->usage_page;

    /* Eat the release of any key we consumed on press */
    if (!ev->state && kc == s_consumed_kc && page == s_consumed_page) {
        s_consumed_kc   = 0;
        s_consumed_page = 0;
        return ZMK_EV_EVENT_HANDLED;
    }

    if (!kh_is_active()) return ZMK_EV_EVENT_BUBBLE;

    /* Let modifier keycodes through so mod-morph keeps working */
    if (kc >= 0xE0 && kc <= 0xE7) return ZMK_EV_EVENT_BUBBLE;

    /* Process navigation on key press; swallow all other keys */
    if (ev->state) {
        /* ctrl bit in HID modifier byte: LCTRL=bit0, RCTRL=bit4 */
        bool ctrl = (zmk_hid_get_mods() & 0x11) != 0;

        if      (kc == 0x52 || kc == 0x0E || (kc == 0x13 && ctrl))
            k_work_submit_to_queue(zmk_display_work_q(), &scroll_up_work);
        else if (kc == 0x51 || kc == 0x0D || (kc == 0x11 && ctrl))
            k_work_submit_to_queue(zmk_display_work_q(), &scroll_down_work);
        else if (kc == 0x29)
            k_work_submit_to_queue(zmk_display_work_q(), &toggle_work);
        /* all other keys: fall through and get swallowed */
    }

    s_consumed_kc   = kc;
    s_consumed_page = page;
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(kh_key_nav, kh_key_nav);
ZMK_SUBSCRIPTION(kh_key_nav, zmk_keycode_state_changed);
