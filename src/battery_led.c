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

/*
 * ZMK has no clean, public "peripheral N just connected/disconnected" event
 * on the central - the one that exists (zmk_split_peripheral_status_changed)
 * is local to each peripheral, about its own link, not exposed centrally
 * with a source index. So "connected" here is inferred: a side counts as
 * connected as long as we've heard a battery report from it recently.
 * CONFIG_ZMK_BATTERY_REPORT_INTERVAL is 30s (see totem_left.conf /
 * totem_right.conf), so 3 missed reports is a safe margin against one
 * arriving a little late without mistaking a real disconnect/sleep for
 * still-connected. The resulting lag (up to ~90s to notice asleep, up to
 * ~30s to notice awake again) is fine for "is it on for the night", which is
 * what this is actually for - not real-time status.
 */
#define TOTEM_DISCONNECT_TIMEOUT_MS (3 * 30000)

/* Software PWM for the "dim" state: toggles the LED at a rate fast enough
 * to look like a steady dim glow rather than a blink, without needing real
 * PWM hardware wired up. */
#define TOTEM_DIM_TICK_MS 3
#define TOTEM_DIM_PERIOD_TICKS 8
#define TOTEM_DIM_ON_TICKS 1

static const struct gpio_dt_spec left_led = GPIO_DT_SPEC_GET(DT_NODELABEL(left_battery_led), gpios);
static const struct gpio_dt_spec right_led = GPIO_DT_SPEC_GET(DT_NODELABEL(right_battery_led), gpios);

enum led_mode { LED_OFF, LED_DIM, LED_BRIGHT };

struct side_state {
    bool connected;
    int64_t last_seen_uptime;
    uint8_t last_pct;
};

static struct side_state left_state;
static struct side_state right_state;

static enum led_mode mode_for(const struct side_state *state) {
    if (!state->connected) {
        return LED_OFF;
    }
    return (state->last_pct < TOTEM_BATTERY_CHARGED_PCT) ? LED_BRIGHT : LED_DIM;
}

static void apply_led(const struct gpio_dt_spec *led, enum led_mode mode, uint32_t tick) {
    bool on;

    switch (mode) {
    case LED_BRIGHT:
        on = true;
        break;
    case LED_DIM:
        on = (tick % TOTEM_DIM_PERIOD_TICKS) < TOTEM_DIM_ON_TICKS;
        break;
    case LED_OFF:
    default:
        on = false;
        break;
    }

    gpio_pin_set_dt(led, on);
}

/*
 * Battery-check blink sequence: flashes the left LED once per 10% of left
 * charge (rounded, minimum 1 so a press always visibly does something),
 * pauses, then does the same for the right LED. While this runs, the dim
 * tick handler below leaves the LEDs alone (see bc_state.running).
 */
enum bc_step { BC_LEFT_ON, BC_LEFT_OFF, BC_GAP, BC_RIGHT_ON, BC_RIGHT_OFF, BC_DONE };

static struct {
    bool running;
    enum bc_step step;
    int left_remaining;
    int right_remaining;
} bc_state;

#define BC_FLASH_ON_MS 250
#define BC_FLASH_OFF_MS 200
#define BC_SIDE_GAP_MS 600

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
        bc_state.right_remaining = flashes_for(right_state.last_pct);
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
        /* Don't restore the LEDs here - the dim tick handler runs every few
         * ms and will pick the right state back up on its own now that
         * `running` is clear. */
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
    bc_state.left_remaining = flashes_for(left_state.last_pct);
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

/* Runs continuously: ages out "connected" after a period of silence, and
 * (unless a battery check is actively blinking) applies each side's LED
 * mode - off/dim/bright - ticking the dim duty cycle along the way. */
static uint32_t dim_tick_count;

static void led_tick_handler(struct k_work *work) {
    dim_tick_count++;

    const int64_t now = k_uptime_get();
    if (left_state.connected && (now - left_state.last_seen_uptime) > TOTEM_DISCONNECT_TIMEOUT_MS) {
        left_state.connected = false;
    }
    if (right_state.connected && (now - right_state.last_seen_uptime) > TOTEM_DISCONNECT_TIMEOUT_MS) {
        right_state.connected = false;
    }

    if (!bc_state.running) {
        apply_led(&left_led, mode_for(&left_state), dim_tick_count);
        apply_led(&right_led, mode_for(&right_state), dim_tick_count);
    }

    k_work_schedule(k_work_delayable_from_work(work), K_MSEC(TOTEM_DIM_TICK_MS));
}

static K_WORK_DELAYABLE_DEFINE(led_tick_work, led_tick_handler);

static int battery_led_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const int64_t now = k_uptime_get();

    switch (ev->source) {
    case TOTEM_LEFT_SOURCE:
        left_state.last_pct = ev->state_of_charge;
        left_state.last_seen_uptime = now;
        left_state.connected = true;
        break;
    case TOTEM_RIGHT_SOURCE:
        right_state.last_pct = ev->state_of_charge;
        right_state.last_seen_uptime = now;
        right_state.connected = true;
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

    k_work_schedule(&led_tick_work, K_MSEC(TOTEM_DIM_TICK_MS));

    return 0;
}

SYS_INIT(battery_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

ZMK_LISTENER(battery_led, battery_led_listener);
ZMK_SUBSCRIPTION(battery_led, zmk_peripheral_battery_state_changed);
