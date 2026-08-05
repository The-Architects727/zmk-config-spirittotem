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

/*
 * ZMK's own kscan matrix driver (app/module/drivers/kscan/kscan_gpio_matrix.c)
 * does its debounce/re-scan polling via a k_work_delayable submitted with
 * plain k_work_reschedule() - which, like plain k_work_submit()/
 * k_work_schedule() below, targets the shared default system workqueue.
 * flash_and_sleep's k_msleep() calls used to run on that same workqueue via
 * K_WORK_DEFINE/K_WORK_DELAYABLE_DEFINE, blocking it for hundreds of
 * milliseconds at a time - starving kscan's own "poll quickly while a key
 * is still settling" loop at the worst possible moment (while the gesture
 * keys were still being read) and very likely the real cause of both the
 * spurious repeated characters and the failure to wake afterwards, neither
 * of which had anything to do with how long the keys were actually held.
 * A dedicated queue, with its own thread, means blocking here can never
 * again compete with kscan (or anything else) for the system workqueue.
 */
static struct k_work_q sleep_work_q;
K_THREAD_STACK_DEFINE(sleep_work_q_stack, 2048);

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
 * for a minute, brings it back").
 *
 * A first version tried to fix this by waiting (with a timeout) for release
 * *after* detecting the press instead of before flashing at all. In testing
 * that produced a burst of repeated characters starting only after the
 * flash finished and lasting close to the full timeout, regardless of how
 * briefly the keys were actually tapped - the giveaway that the timeout was
 * being hit every time, not because of a long hold, but because the wait
 * loop (like the flash before it) was itself running on the shared system
 * workqueue and monopolizing it, so kscan's own debounce/re-scan work
 * (queued on that same workqueue - see sleep_work_q above) couldn't run and
 * report the actual release no matter how quickly it happened, until the
 * timeout finally gave up waiting and this returned. Between the dedicated
 * queue above (so this can never again block kscan) and triggering only
 * after sleep_gesture_listener below has already observed the full
 * press-then-release itself, there's nothing left to wait for here at all:
 * by the time this handler runs, the keys are already confirmed up.
 */
static atomic_t sleep_in_progress = ATOMIC_INIT(0);

static void sleep_work_handler(struct k_work *work) {
    if (!atomic_cas(&sleep_in_progress, 0, 1)) {
        return;
    }
    /* Quick and bright rather than the auto-sleep timing below: this is a
     * deliberate gesture the user is actively watching, and there's no
     * reason to draw it out now that it only starts after release. */
    do_flash(3, 80, 60);
    zmk_pm_soft_off();
}

static K_WORK_DEFINE(sleep_work, sleep_work_handler);

/*
 * Manual sleep gesture: press Esc+Z (left) or Slash+Minus (right) together,
 * then let go - whichever pair actually exists on this half's own kscan
 * matrix; the other pair's positions simply never fire here, so this same
 * code works unmodified on both builds.
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
 * Triggers on release, not press: `armed` latches true the moment both
 * positions of a pair are simultaneously down, and the actual sleep_work
 * only gets submitted once both have gone back up. This means (a) by the
 * time sleep_work_handler runs, the keys are already confirmed released -
 * no race, no timeout, nothing left to wait for before calling
 * zmk_pm_soft_off() - and (b) whatever the keys' normal Escape/Z or
 * Slash/Minus output was during the brief press is over by the time
 * anything else happens, instead of continuing to repeat for as long as a
 * flash-then-sleep sequence takes to run.
 */
#define SLEEP_GESTURE_POS_LEFT_1 20  /* Escape */
#define SLEEP_GESTURE_POS_LEFT_2 21  /* Z */
#define SLEEP_GESTURE_POS_RIGHT_1 30 /* Slash */
#define SLEEP_GESTURE_POS_RIGHT_2 31 /* Minus */

static bool pos_left_1, pos_left_2, pos_right_1, pos_right_2;
static bool left_armed, right_armed;

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

    if (pos_left_1 && pos_left_2) {
        left_armed = true;
    }
    if (pos_right_1 && pos_right_2) {
        right_armed = true;
    }

    if (left_armed && !pos_left_1 && !pos_left_2) {
        left_armed = false;
        k_work_submit_to_queue(&sleep_work_q, &sleep_work);
    }
    if (right_armed && !pos_right_1 && !pos_right_2) {
        right_armed = false;
        k_work_submit_to_queue(&sleep_work_q, &sleep_work);
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
        k_work_schedule_for_queue(&sleep_work_q, &auto_sleep_work, K_MSEC(TOTEM_AUTO_SLEEP_TIMEOUT_MS));
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

    k_work_queue_init(&sleep_work_q);
    k_work_queue_start(&sleep_work_q, sleep_work_q_stack, K_THREAD_STACK_SIZEOF(sleep_work_q_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

    return 0;
}

SYS_INIT(totem_sleep_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
