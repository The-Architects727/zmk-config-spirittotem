/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_battery_check

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
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

/* Battery-check blink timing, in milliseconds. */
#define BC_FLASH_ON_MS 250
#define BC_FLASH_OFF_MS 200
#define BC_SIDE_GAP_MS 600

static const struct gpio_dt_spec left_led = GPIO_DT_SPEC_GET(DT_NODELABEL(left_battery_led), gpios);
static const struct gpio_dt_spec right_led = GPIO_DT_SPEC_GET(DT_NODELABEL(right_battery_led), gpios);

static uint8_t last_left_pct = 0;
static uint8_t last_right_pct = 0;

static void restore_led(const struct gpio_dt_spec *led, uint8_t state_of_charge) {
    const bool low_battery = state_of_charge < TOTEM_BATTERY_CHARGED_PCT;
    int rc = gpio_pin_set_dt(led, low_battery);

    if (rc != 0) {
        LOG_ERR("Failed to set battery LED: %d", rc);
    }
}

/*
 * Battery-check blink sequence: flashes the left LED once per 10% of left
 * charge (rounded, minimum 1 so a press always visibly does something),
 * pauses, then does the same for the right LED, then restores both LEDs to
 * their normal low-battery indicator state.
 */
enum bc_step { BC_LEFT_ON, BC_LEFT_OFF, BC_GAP, BC_RIGHT_ON, BC_RIGHT_OFF, BC_DONE };

static struct {
    bool running;
    enum bc_step step;
    int left_remaining;
    int right_remaining;
} bc_state;

static int flashes_for(uint8_t pct) {
    int flashes = (pct + 5) / 10;

    if (flashes > 10) {
        flashes = 10;
    }
    if (flashes < 1) {
        flashes = 1;
    }
    return flashes;
}

static void battery_check_work_handler(struct k_work *work) {
    switch (bc_state.step) {
    case BC_LEFT_ON:
        gpio_pin_set_dt(&left_led, 1);
        bc_state.step = BC_LEFT_OFF;
        k_work_schedule(k_work_delayable_from_work(work), K_MSEC(BC_FLASH_ON_MS));
        return;

    case BC_LEFT_OFF:
        gpio_pin_set_dt(&left_led, 0);
        bc_state.left_remaining--;
        if (bc_state.left_remaining > 0) {
            bc_state.step = BC_LEFT_ON;
            k_work_schedule(k_work_delayable_from_work(work), K_MSEC(BC_FLASH_OFF_MS));
        } else {
            bc_state.step = BC_GAP;
            k_work_schedule(k_work_delayable_from_work(work), K_MSEC(BC_SIDE_GAP_MS));
        }
        return;

    case BC_GAP:
        bc_state.right_remaining = flashes_for(last_right_pct);
        bc_state.step = BC_RIGHT_ON;
        k_work_schedule(k_work_delayable_from_work(work), K_NO_WAIT);
        return;

    case BC_RIGHT_ON:
        gpio_pin_set_dt(&right_led, 1);
        bc_state.step = BC_RIGHT_OFF;
        k_work_schedule(k_work_delayable_from_work(work), K_MSEC(BC_FLASH_ON_MS));
        return;

    case BC_RIGHT_OFF:
        gpio_pin_set_dt(&right_led, 0);
        bc_state.right_remaining--;
        if (bc_state.right_remaining > 0) {
            bc_state.step = BC_RIGHT_ON;
            k_work_schedule(k_work_delayable_from_work(work), K_MSEC(BC_FLASH_OFF_MS));
        } else {
            bc_state.step = BC_DONE;
            k_work_schedule(k_work_delayable_from_work(work), K_NO_WAIT);
        }
        return;

    case BC_DONE:
    default:
        restore_led(&left_led, last_left_pct);
        restore_led(&right_led, last_right_pct);
        bc_state.running = false;
        return;
    }
}

static K_WORK_DELAYABLE_DEFINE(battery_check_work, battery_check_work_handler);

static int battery_check_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    if (bc_state.running) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    bc_state.running = true;
    bc_state.step = BC_LEFT_ON;
    bc_state.left_remaining = flashes_for(last_left_pct);
    k_work_schedule(&battery_check_work, K_NO_WAIT);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int battery_check_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api battery_check_driver_api = {
    .binding_pressed = battery_check_pressed,
    .binding_released = battery_check_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
#define BATTERY_CHECK_INST(n)                                                                     \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &battery_check_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BATTERY_CHECK_INST)
#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

static int battery_led_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (ev->source) {
    case TOTEM_LEFT_SOURCE:
        last_left_pct = ev->state_of_charge;
        if (!bc_state.running) {
            restore_led(&left_led, last_left_pct);
        }
        break;
    case TOTEM_RIGHT_SOURCE:
        last_right_pct = ev->state_of_charge;
        if (!bc_state.running) {
            restore_led(&right_led, last_right_pct);
        }
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
