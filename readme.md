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
  [`src/battery_led.c`](src/battery_led.c). Each LED has three states: **off**
  (that half hasn't reported in ~60s — asleep or disconnected), **dim** (a
  low-duty-cycle software pulse — connected, battery fine), or **bright**
  (connected, battery low). ZMK doesn't expose a clean central-side "is
  peripheral N connected" event, so "connected" here is inferred from battery
  report freshness (reports arrive every 10s; 6 missed reports = assume
  asleep). That means up to ~60s of lag noticing a disconnect and ~10s
  noticing a reconnect — fine for "is it on for the night", not meant for
  real-time status. The interval is short specifically because left and right
  run this report timer independently and unsynchronized, so their "assume
  asleep" moments can visibly disagree by up to one full report interval; a
  shorter interval bounds how far apart the two LEDs can land. The 6-miss
  tolerance (up from an original 3) exists because the dongle juggles two
  simultaneous peripheral connections, and an occasional single report
  landing late is normal BLE behavior on that kind of link, not a real
  disconnect - 3 misses was tight enough that it periodically flickered the
  LED off and back on for no reason. `CONFIG_ZMK_IDLE_TIMEOUT` is also raised
  to effectively never (see `totem_left.conf`/`totem_right.conf`) so a lull
  in typing doesn't pause battery reporting and trip this same heuristic.
- **On-demand battery check.** Hold the Fun layer, tap the far outer-left
  pinky key to blink out each half's charge in 10% steps (left LED, pause,
  right LED) — see [`src/battery_led.c`](src/battery_led.c) for the behavior
  driver and blink sequencer.
- **Manual sleep gesture, with a warning flash.** Press Esc + Z together then
  let go to sleep the left half, or Slash + Minus for the right half - each
  half decides for itself and acts independently, so you can sleep just one
  side or both. Implemented in [`src/totem_sleep.c`](src/totem_sleep.c) as a
  plain listener on `zmk_position_state_changed` for those four positions
  (Escape/Z on the left, Slash/Minus on the right - whichever pair actually
  exists on that half's own kscan matrix; the other pair's positions simply
  never fire on that build). It only acts on the full press-then-release
  cycle, not on press: a pair latches "armed" the moment both its keys are
  simultaneously down, and the flash+sleep only fires once both have gone
  back up. That's not cosmetic - `zmk_pm_soft_off()` (`app/src/pm.c` in the
  ZMK source) re-arms the kscan matrix as a wakeup source right before
  powering off, and if the trigger keys are still held at that exact moment,
  that's a wake-detect line already active when the wakeup gets armed - a
  known nRF52 GPIO SENSE/LATCH hazard that can leave the wake mechanism
  stuck until a true power-on-reset. Triggering only after release means the
  keys are already confirmed up by the time any of this runs, so there's
  nothing to wait for and no timeout needed - the first version of this fix
  used a wait-with-timeout *after* the press instead, but the timeout itself
  was just a delayed way to still call `zmk_pm_soft_off()` while the keys
  were held, so it didn't actually fix anything. Once triggered, it flashes
  the XIAO nRF52840's built-in green LED (`led1` - separate hardware from
  the dongle's D0/D1 battery LEDs, already present on the board, no wiring
  needed) 3 quick times, then sleeps. Any keypress on a half wakes it back up
  afterwards (via `wakeup-source` on `kscan0` and a
  `zmk,soft-off-wakeup-sources` node, both in
  [`totem.dtsi`](config/boards/shields/totem/totem.dtsi)) - each half sleeps
  and wakes independently, there's no way for a sleeping battery-powered
  peripheral to be woken remotely. This is separate from and doesn't require
  `CONFIG_ZMK_SLEEP` (automatic idle-timeout sleep, which stays off) - it
  only happens when you deliberately trigger the gesture. An atomic guard
  also makes sure only one flash+sleep sequence can ever be in flight at a
  time, in case switch bounce re-fires the listener while one's already
  running.
  - **Runs on its own dedicated workqueue, not the default system one.**
    ZMK's own kscan matrix driver
    (`app/module/drivers/kscan/kscan_gpio_matrix.c`) does its debounce/
    re-scan polling via a `k_work_delayable` on that same shared system
    workqueue. The flash sequence's `k_msleep()` calls, if run there too
    (as an earlier version did), block that single thread for hundreds of
    milliseconds at a time - `k_msleep()` yields the CPU to *other threads*
    during that time, but it doesn't let the *same* workqueue move on to
    its next queued item, so kscan's own scan processing (for every key,
    not just the gesture ones) is stuck behind it the whole time. In
    testing this produced a burst of spurious repeated characters right
    after the flash sequence released the queue, and blocked other typing
    entirely while it was running - both symptoms of kscan being starved,
    not of how long any key was actually held. `sleep_work_q` (a small
    dedicated queue with its own thread, started in `totem_sleep_init`)
    means this code can never again compete with kscan for the same
    thread.
  - **This used to be a ZMK combo** (evaluated centrally, with the trigger
    relayed back down to the peripheral over BLE via a custom
    `BEHAVIOR_LOCALITY_GLOBAL` behavior). That path turned out to be
    unreliable in practice on this hardware: it took fixing two real,
    confirmed bugs along the way -
    1. the devicetree node backing the custom behavior had too long a name;
       `GLOBAL`/`EVENT_SOURCE` behaviors get relayed central-to-peripheral
       over BLE by looking up the target's device name in a fixed `char[16]`
       buffer (`zmk_split_transport_central_command.data.invoke_behavior.behavior_dev`,
       see `app/include/zmk/split/transport/types.h` in the ZMK source), and
       a longer name gets silently truncated in transit and matches nothing
       on the peripheral - this is why stock `&soft_off`'s own node is named
       the cryptic `z_so_off` rather than something descriptive;
    2. the peripheral-side handler for a relayed invocation
       (`split_svc_run_behavior` in `app/src/split/bluetooth/service.c`) is a
       Bluetooth GATT write callback - the BLE host stack's own thread, not
       a normal application thread - and the behavior was blocking that
       thread for the full ~1.5s of the flash sequence via `k_msleep()`.

    and even after both fixes it *still* didn't fire reliably, most likely
    a dropped or delayed packet somewhere in the relay itself with no way to
    confirm further without a serial debug probe. Listening locally
    sidesteps the whole relay: no BLE round-trip, no GATT callback, nothing
    in the path that can silently drop a command.
  - **Trade-off:** unlike a real combo, this doesn't suppress the keys'
    normal output - Escape/Z or Slash/Minus still get sent to the host as
    usual when you do this, same as any other keypress. It's a deliberate
    two-key hold you wouldn't do while typing normally, so a stray Escape/z
    or /- landing in whatever's focused right before that half sleeps is an
    accepted one-time side effect of the simpler, more reliable approach.
- **Auto-sleep when the dongle disappears.** The other half of
  [`src/totem_sleep.c`](src/totem_sleep.c) (gated by `CONFIG_TOTEM_SLEEP_WARN`,
  which depends on `!ZMK_SPLIT_ROLE_CENTRAL` - left/right only) subscribes to
  `zmk_split_peripheral_status_changed` — a real, per-peripheral "am I
  connected to my central" event that ZMK already raises on connect/disconnect
  (see `app/src/split/bluetooth/peripheral.c`), unlike the central-side gap
  that `battery_led.c`'s connection heuristic has to work around. If a half
  goes 5 minutes without reaching the dongle - e.g. you powered the dongle off
  for the night - it flashes the same onboard LED and puts itself to sleep
  automatically. Slower and brighter than the gesture's flash (nobody's
  sitting there holding a key down waiting for this one, so the wakeup-source
  hazard above doesn't apply, and it needs to be noticeable to someone who
  wasn't watching) and doesn't wait on anything before calling
  `zmk_pm_soft_off()`, since no keys are involved in this trigger at all.
  `k_work_schedule` (not `reschedule`) is used deliberately so repeated
  disconnect events from failed reconnect attempts don't keep pushing the
  deadline back; only the first one starts the clock, and reconnecting
  cancels it.

## How the battery data actually gets to the dongle

ZMK's split transport already carries a battery-percentage event
(`ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT`) over the same
peripheral→central link used for key events — it's not literally packed into
each key-position packet, but it rides the same transport as its own event
type, updated every `CONFIG_ZMK_BATTERY_REPORT_INTERVAL` seconds (10s here,
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
