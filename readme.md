# Totem (custom build)

A copy of the [TOTEM](https://github.com/eigatech/zmk-config) split keyboard
config (dongle variant), adapted to add per-half battery-charge awareness on
the dongle.

Same key layout/matrix and same controller (Seeed XIAO nRF52840 BLE) as the
`totem`/`totem-dongle`/`totem-prospector` branches this was copied from. Three
firmware images come out of one build: `totem_left`, `totem_right`, and
`totem_dongle` (plus `settings_reset`), built together from this single repo
via GitHub Actions — that's the standard way ZMK config repos work, so the
per-controller files live side by side here rather than in separate repos,
distinguished by filename (`totem_left.*`, `totem_right.*`, `totem_dongle.*`).

## What's different from the source repos

- **Board id updated to `xiao_ble//zmk`.** The original `totem`/`totem-dongle`
  branches target `board: seeeduino_xiao_ble`, a board id that's been removed
  from current ZMK/Zephyr (it doesn't exist anywhere in `zmkfirmware/zmk` or
  its pinned `zmkfirmware/zephyr` fork as of this writing). `totem-prospector`
  had already moved to `xiao_ble//zmk`, so this build follows that — it's the
  variant that actually resolves against `revision: main`. That board already
  wires up a battery voltage-divider (`vbatt`) on every XIAO BLE automatically,
  so no devicetree work was needed just to read the ADC.
- **Battery-charge LEDs on the dongle.** `left_battery_led`/`right_battery_led`
  GPIOs on the dongle's D0/D1 pins (see
  [`totem_dongle.overlay`](config/boards/shields/totem/totem_dongle.overlay)),
  driven by a small custom listener in
  [`src/battery_led.c`](src/battery_led.c).
- **On-demand battery check.** Hold the Fun layer, tap the far outer-left
  pinky key to blink out each half's charge in 10% steps (left LED, pause,
  right LED) — see [`src/battery_led.c`](src/battery_led.c) for the behavior
  driver and blink sequencer.

## How the battery data actually gets to the dongle

ZMK's split transport already carries a battery-percentage event
(`ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT`) over the same
peripheral→central link used for key events — it's not literally packed into
each key-position packet, but it rides the same transport as its own event
type, updated every `CONFIG_ZMK_BATTERY_REPORT_INTERVAL` seconds (30s here,
see `totem_left.conf`/`totem_right.conf`). That's the standard, tested ZMK
mechanism; reusing it (rather than hand-rolling a custom protocol that embeds
battery bits inside every key packet) means no changes to ZMK core were
needed. The dongle must have
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y` set (already done in
`totem_dongle.conf`) or it never receives these events at all.

`src/battery_led.c` subscribes to `zmk_peripheral_battery_state_changed` and
turns on the corresponding LED when the reported state-of-charge drops below
34% — the percentage ZMK's own `battery-voltage-divider` driver computes for
~3.7V (it maps millivolts to percent via `mv * 2 / 15 - 459`, and ZMK never
transmits raw millivolts over the split link, only this percentage). So
"charged" here really does mean "the half reported >=~3.7V", just expressed
in the units ZMK actually sends. This intentionally keeps the peripherals on
ZMK's stock, well-tested battery driver rather than a custom one — all the
LED/blink logic lives on the dongle, where it's easy to change without
touching left/right firmware at all.

## Wiring and setup notes

- **LED wiring:** anode (+ resistor) to D0 (left) / D1 (right), cathode to
  GND. If you wire it the other way, flip `GPIO_ACTIVE_HIGH` to
  `GPIO_ACTIVE_LOW` in `totem_dongle.overlay`.
- **Left/right assignment is by pairing order, not hardware identity.** The
  dongle assigns peripheral "source" index 0 to whichever half it bonds to
  first. Flash and power on the left half first when initially pairing so it
  becomes source 0 (mapped to D0); if the LEDs end up swapped, either re-pair
  in the other order (flash `settings_reset` to both halves first) or swap
  the `TOTEM_LEFT_SOURCE`/`TOTEM_RIGHT_SOURCE` values in
  `src/battery_led.c`.
- **Threshold tuning:** `TOTEM_BATTERY_CHARGED_PCT` in `src/battery_led.c`
  controls the cutoff (34 ≈ 3.7V per ZMK's own curve). Adjust if you want a
  different voltage cutoff.
- Update `url` in `config/totem.zmk.yml` / `config/info.json` once this is
  pushed to your own GitHub repo, and swap the `west.yml`/build workflow
  remotes if you fork rather than start fresh.

## Building

Push this to a GitHub repo and GitHub Actions (`.github/workflows/build.yml`)
will build all four images via ZMK's `build-user-config.yml` reusable
workflow — no local toolchain required. Flash `totem_left`/`totem_right` to
each half and `totem_dongle` to the receiver.
