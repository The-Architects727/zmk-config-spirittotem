/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/pm.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Runs on the split peripherals (left/right) only - see the Kconfig gate on
 * CONFIG_TOTEM_SLEEP_WARN. Uses the XIAO nRF52840's built-in green LED
 * (led1, already defined by the board itself - unrelated to the dongle's
 * D0/D1 battery LEDs) as a "going to sleep now" warning, shared by both
 * sleep triggers below.
 */
static const struct gpio_dt_spec onboard_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void do_flash(int flashes, int on_ms, int off_ms) {
    for (int i = 0; i < flashes; i++) {
        gpio_pin_set_dt(&onboard_led, 1);
        k_msleep(on_ms);
        gpio_pin_set_dt(&onboard_led, 0);
        k_msleep(off_ms);
    }
}

/*
 * zmk_pm_soft_off() (app/src/pm.c) re-arms the kscan matrix as a wakeup
 * source right before powering off. If the physical keys that triggered
 * sleep are still being held at that exact moment, that's a level already
 * active when the wake-detect gets armed - a known nRF52 GPIO SENSE/LATCH
 * hazard that can leave the wake mechanism stuck until a true
 * power-on-reset (matching "only the reset button, or draining the battery
 * for a minute, brings it back"). Guards both sleep triggers against
 * calling zmk_pm_soft_off() while any tracked key is still down.
 */
static atomic_t sleep_in_progress = ATOMIC_INIT(0);

static bool any_gesture_key_down(void);

static void wait_for_release_and_sleep(void) {
    for (int waited_ms = 0; any_gesture_key_down() && waited_ms < 3000; waited_ms += 20) {
        k_msleep(20);
    }
    zmk_pm_soft_off();
}

static void sleep_work_handler(struct k_work *work) {
    if (!atomic_cas(&sleep_in_progress, 0, 1)) {
        return;
    }
    /* Quick and bright rather than the auto-sleep timing below: this is a
     * deliberate gesture the user is actively watching, and a fast flash
     * minimizes how long they're still holding the trigger keys down -
     * both for OS key-repeat (nothing here suppresses the keys' normal
     * output - see the gesture comment further down) and for the wakeup-
     * source hazard above. */
    do_flash(3, 80, 60);
    wait_for_release_and_sleep();
}

static K_WORK_DEFINE(sleep_work, sleep_work_handler);

/*
 * Manual sleep gesture: hold Esc+Z (left) or Slash+Minus (right) - whichever
 * pair actually exists on this half's own kscan matrix; the other pair's
 * positions simply never fire here, so this same code works unmodified on
 * both builds.
 *
 * This used to be a ZMK combo (evaluated centrally, with the trigger relayed
 * back down to the peripheral over BLE via a custom GLOBAL-locality
 * behavior). That turned out to be unreliable in practice: it took fixing
 * two real bugs (a truncated split-transport behavior name, then a
 * blocking call inside the Bluetooth GATT write callback that receives the
 * relayed command) and it *still* didn't fire reliably, most likely a
 * dropped/delayed BLE packet somewhere in the central-to-peripheral relay
 * itself with no way to know for sure without a debug probe. Listening
 * locally sidesteps all of that: physical_layouts.c already raises
 * zmk_position_state_changed on this device for its own keys before
 * anything is sent to the central at all, so there's no relay, no GATT
 * callback, nothing that can drop a packet - each half decides for itself.
 *
 * Trade-off: unlike a real combo, this doesn't suppress the keys' normal
 * output - Escape/Z or Slash/Minus still get sent to the host as usual when
 * you do this, same as any other keypress. It's a deliberate two-key press
 * you wouldn't do while typing normally, so a brief Escape/z or /- landing
 * in whatever's focused is an acceptable one-time side effect - and since
 * sleep now waits for release (see wait_for_release_and_sleep above), it's
 * a single press+release rather than a held-down key, so it shouldn't
 * repeat.
 */
#define SLEEP_GESTURE_POS_LEFT_1 20  /* Escape */
#define SLEEP_GESTURE_POS_LEFT_2 21  /* Z */
#define SLEEP_GESTURE_POS_RIGHT_1 30 /* Slash */
#define SLEEP_GESTURE_POS_RIGHT_2 31 /* Minus */

static bool pos_left_1, pos_left_2, pos_right_1, pos_right_2;

static bool any_gesture_key_down(void) { return pos_left_1 || pos_left_2 || pos_right_1 || pos_right_2; }

static int sleep_gesture_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (ev->position) {
    case SLEEP_GESTURE_POS_LEFT_1:
        pos_left_1 = ev->state;
        break;
    case SLEEP_GESTURE_POS_LEFT_2:
        pos_left_2 = ev->state;
        break;
    case SLEEP_GESTURE_POS_RIGHT_1:
        pos_right_1 = ev->state;
        break;
    case SLEEP_GESTURE_POS_RIGHT_2:
        pos_right_2 = ev->state;
        break;
    default:
        return ZMK_EV_EVENT_BUBBLE;
    }

    if ((pos_left_1 && pos_left_2) || (pos_right_1 && pos_right_2)) {
        k_work_submit(&sleep_work);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_sleep_gesture, sleep_gesture_listener);
ZMK_SUBSCRIPTION(totem_sleep_gesture, zmk_position_state_changed);

/*
 * Auto-sleep when this half can't reach its central (dongle) for 5 minutes -
 * e.g. you powered the dongle off for the night. Subscribes to
 * zmk_split_peripheral_status_changed, a real per-peripheral "am I connected"
 * event ZMK raises on connect/disconnect (app/src/split/bluetooth/peripheral.c).
 * k_work_schedule (not reschedule) is deliberate: repeated disconnect events
 * from failed reconnect attempts don't keep pushing the deadline back, only
 * the first one starts the clock, and reconnecting cancels it. Slower/
 * brighter flash timing than the gesture above is fine here - nobody's
 * holding a key down waiting for it, so the wakeup-source hazard doesn't
 * apply, and this needs to be noticeable to someone who wasn't watching.
 */
#define TOTEM_AUTO_SLEEP_TIMEOUT_MS (5 * 60 * 1000)

static void auto_sleep_work_handler(struct k_work *work) {
    if (!atomic_cas(&sleep_in_progress, 0, 1)) {
        return;
    }
    LOG_INF("Central unreachable for %d minutes, sleeping", TOTEM_AUTO_SLEEP_TIMEOUT_MS / 60000);
    do_flash(3, 300, 200);
    zmk_pm_soft_off();
}

static K_WORK_DELAYABLE_DEFINE(auto_sleep_work, auto_sleep_work_handler);

static int auto_sleep_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev = as_zmk_split_peripheral_status_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->connected) {
        k_work_cancel_delayable(&auto_sleep_work);
    } else {
        k_work_schedule(&auto_sleep_work, K_MSEC(TOTEM_AUTO_SLEEP_TIMEOUT_MS));
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_auto_sleep, auto_sleep_listener);
ZMK_SUBSCRIPTION(totem_auto_sleep, zmk_split_peripheral_status_changed);

static int totem_sleep_init(void) {
    if (!gpio_is_ready_dt(&onboard_led)) {
        LOG_ERR("Onboard LED GPIO is not ready");
        return -ENODEV;
    }

    gpio_pin_configure_dt(&onboard_led, GPIO_OUTPUT_INACTIVE);

    return 0;
}

SYS_INIT(totem_sleep_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
