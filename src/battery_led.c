/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * ZMK only ever transmits split peripheral battery level as a 0-100 state-
 * of-charge percentage (see zephyr's zmk,battery-voltage-divider driver and
 * lithium_ion_mv_to_pct()), never raw millivolts. That driver maps
 * millivolts to percent via `mv * 2 / 15 - 459`, so a "charged" threshold of
 * 3700mV works out to a state-of-charge of 34%. This keeps the ">=3.7V is
 * charged" behavior consistent with what the peripheral actually measured,
 * without needing a custom battery driver.
 */
#define TOTEM_BATTERY_CHARGED_PCT 34

/* Split peripheral "source" index is assigned in bonding order (whichever
 * half pairs to the dongle first becomes source 0). Pair left before right
 * (or swap the two cases below) so this matches your hardware. */
#define TOTEM_LEFT_SOURCE 0
#define TOTEM_RIGHT_SOURCE 1

static const struct gpio_dt_spec left_led = GPIO_DT_SPEC_GET(DT_NODELABEL(left_battery_led), gpios);
static const struct gpio_dt_spec right_led = GPIO_DT_SPEC_GET(DT_NODELABEL(right_battery_led), gpios);

static void set_low_battery_led(const struct gpio_dt_spec *led, uint8_t state_of_charge) {
    const bool low_battery = state_of_charge < TOTEM_BATTERY_CHARGED_PCT;
    int rc = gpio_pin_set_dt(led, low_battery);

    if (rc != 0) {
        LOG_ERR("Failed to set battery LED: %d", rc);
    }
}

static int battery_led_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (ev->source) {
    case TOTEM_LEFT_SOURCE:
        set_low_battery_led(&left_led, ev->state_of_charge);
        break;
    case TOTEM_RIGHT_SOURCE:
        set_low_battery_led(&right_led, ev->state_of_charge);
        break;
    default:
        LOG_WRN("Battery update from unexpected peripheral source %d", ev->source);
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int battery_led_init(void) {
    if (!gpio_is_ready_dt(&left_led) || !gpio_is_ready_dt(&right_led)) {
        LOG_ERR("Battery LED GPIOs are not ready");
        return -ENODEV;
    }

    gpio_pin_configure_dt(&left_led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&right_led, GPIO_OUTPUT_INACTIVE);

    return 0;
}

SYS_INIT(battery_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

ZMK_LISTENER(battery_led, battery_led_listener);
ZMK_SUBSCRIPTION(battery_led, zmk_peripheral_battery_state_changed);
