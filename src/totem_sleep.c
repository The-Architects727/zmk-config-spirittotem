/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_totem_sleep

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
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

#define SLEEP_WARN_FLASHES 3
#define SLEEP_WARN_ON_MS 150
#define SLEEP_WARN_OFF_MS 150

static void flash_and_sleep(void) {
    for (int i = 0; i < SLEEP_WARN_FLASHES; i++) {
        gpio_pin_set_dt(&onboard_led, 1);
        k_msleep(SLEEP_WARN_ON_MS);
        gpio_pin_set_dt(&onboard_led, 0);
        k_msleep(SLEEP_WARN_OFF_MS);
    }
    zmk_pm_soft_off();
}

/*
 * Custom zero-param behavior used by the sleep combo in totem.keymap
 * instead of stock &soft_off directly, so the flash happens no matter which
 * trigger fires. Same GLOBAL locality as &soft_off itself, so it still
 * relays to every connected peripheral the way the combo needs (the combo
 * requires keys from both halves at once, so there's no scenario where only
 * one half should sleep from it anyway).
 */
static int totem_sleep_pressed(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    flash_and_sleep();
    return ZMK_BEHAVIOR_OPAQUE;
}

static int totem_sleep_released(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api totem_sleep_driver_api = {
    .binding_pressed = totem_sleep_pressed,
    .binding_released = totem_sleep_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
#define TOTEM_SLEEP_INST(n)                                                                        \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &totem_sleep_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TOTEM_SLEEP_INST)
#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

/*
 * Auto-sleep when this half can't reach its central (dongle) for 5 minutes -
 * e.g. you powered the dongle off for the night. Subscribes to
 * zmk_split_peripheral_status_changed, a real per-peripheral "am I connected"
 * event ZMK raises on connect/disconnect (app/src/split/bluetooth/peripheral.c).
 * k_work_schedule (not reschedule) is deliberate: repeated disconnect events
 * from failed reconnect attempts don't keep pushing the deadline back, only
 * the first one starts the clock, and reconnecting cancels it.
 */
#define TOTEM_AUTO_SLEEP_TIMEOUT_MS (5 * 60 * 1000)

static void auto_sleep_work_handler(struct k_work *work) {
    LOG_INF("Central unreachable for %d minutes, sleeping", TOTEM_AUTO_SLEEP_TIMEOUT_MS / 60000);
    flash_and_sleep();
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
