# Totem (custom build)

A copy of the [TOTEM](https://github.com/eigatech/zmk-config) split keyboard
config (dongle variant), adapted with per-half battery-charge LEDs on the
dongle and independent sleep management for each half.

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
  from current ZMK/Zephyr. `totem-prospector` had already moved to
  `xiao_ble//zmk`, so this build follows that — it's the variant that
  actually resolves against `revision: main`. That board already wires up a
  battery voltage-divider (`vbatt`) on every XIAO BLE automatically, so no
  devicetree work was needed just to read the ADC.

- **Battery-charge LEDs on the dongle.** `left_battery_led`/`right_battery_led`
  GPIOs on the dongle's D0/D1 pins (see
  [`totem_dongle.overlay`](config/boards/shields/totem/totem_dongle.overlay)),
  driven by [`src/battery_led.c`](src/battery_led.c). Each LED has three
  states: **off** (asleep or disconnected), **dim** (connected, battery
  fine), or **bright** (connected, battery low). See
  [Implementation notes](#implementation-notes) for how "connected" is
  actually determined.

- **On-demand battery check.** Hold the Fun layer, tap the far outer-left
  pinky key to blink out each half's charge in 10% steps (left LED, pause,
  right LED) — see [`src/battery_led.c`](src/battery_led.c) for the behavior
  driver and blink sequencer.

- **Manual sleep gesture, with a warning flash.** Press Esc + Z together then
  let go to sleep the left half, or Slash + Minus for the right half — each
  half decides for itself and acts independently, so you can sleep just one
  side or both. Implemented in [`src/totem_sleep.c`](src/totem_sleep.c): it
  flashes the XIAO nRF52840's built-in green LED (`led1` — separate hardware
  from the dongle's D0/D1 battery LEDs, no wiring needed) 3 quick times, then
  sleeps. Any keypress on a half wakes it back up (via `wakeup-source` on
  `kscan0` and a `zmk,soft-off-wakeup-sources` node, both in
  [`totem.dtsi`](config/boards/shields/totem/totem.dtsi)) — each half sleeps
  and wakes independently, there's no way for a sleeping battery-powered
  peripheral to be woken remotely. This doesn't suppress the keys' normal
  output — Escape/Z or Slash/Minus still get sent to the host as usual — a
  deliberate two-key press you wouldn't do while typing normally, so a brief
  stray character is an accepted side effect. Separate from and doesn't
  require `CONFIG_ZMK_SLEEP` (automatic idle-timeout sleep, which stays
  off). See [Implementation notes](#implementation-notes) for why this isn't
  a ZMK combo and why it triggers on release rather than press.

- **Auto-sleep when the dongle disappears.** The other half of
  [`src/totem_sleep.c`](src/totem_sleep.c) (gated by `CONFIG_TOTEM_SLEEP_WARN`,
  left/right only) subscribes to `zmk_split_peripheral_status_changed` — a
  real, per-peripheral "am I connected to my central" event ZMK raises on
  connect/disconnect. If a half goes 5 minutes without reaching the dongle —
  e.g. you powered the dongle off for the night — it flashes the same
  onboard LED and puts itself to sleep automatically. `k_work_schedule` (not
  `reschedule`) is deliberate: repeated disconnect events from failed
  reconnect attempts don't keep pushing the deadline back, only the first
  one starts the clock, and reconnecting cancels it.

## How the battery data actually gets to the dongle

ZMK's split transport already carries a battery-percentage event over the
same peripheral→central link used for key events, updated every
`CONFIG_ZMK_BATTERY_REPORT_INTERVAL` seconds (10s here, see
`totem_left.conf`/`totem_right.conf`). That's the standard, tested ZMK
mechanism; reusing it (rather than hand-rolling a custom protocol) means no
changes to ZMK core were needed. The dongle must have
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

## Implementation notes

Non-obvious decisions and the reasoning behind them, for anyone touching
this code later.

**"Connected" on the dongle is inferred, not observed directly.** ZMK
doesn't expose a clean central-side "is peripheral N connected" event, so
`battery_led.c` mostly infers it from battery report freshness: 10s report
interval, 6 missed reports (~60s) before assuming asleep. That interval is
short specifically because left and right run this report timer
independently and unsynchronized, so their "assume asleep" moments can
visibly disagree by up to one full report interval — a shorter interval
bounds how far apart the two LEDs can land. The 6-miss tolerance (up from an
original 3) exists because the dongle juggles two simultaneous peripheral
connections, and an occasional single report landing late is normal BLE
behavior on that kind of link, not a real disconnect; 3 misses was tight
enough to periodically flicker the LED off and back on for no reason.
`CONFIG_ZMK_IDLE_TIMEOUT` is also raised to effectively never (see
`totem_left.conf`/`totem_right.conf`) so a lull in typing doesn't pause
battery reporting and trip this same heuristic.

There's a faster path too: ZMK's own central-side BLE disconnect handler
(`split_central_disconnected` in `app/src/split/bluetooth/central.c`)
already synthesizes a battery report of 0% the instant a peripheral's BLE
connection actually drops, for any reason — including a half going to
sleep. `battery_led.c` treats a 0% report as an immediate, authoritative
disconnect signal rather than a real low-battery reading (which would
otherwise light the LED *bright* right as a half goes to sleep — backwards
from the OFF state it should show), so sleeping or disconnecting typically
shows up much faster than the ~60s staleness fallback, with no new
messaging needed from the peripherals at all — ZMK already sends the
signal, `battery_led.c` just has to listen for it correctly.

**The sleep gesture is deliberately not a ZMK combo.** An earlier version
was: evaluated centrally, with the trigger relayed back down to the
peripheral over BLE via a custom `BEHAVIOR_LOCALITY_GLOBAL` behavior. That
path turned out to be unreliable in practice on this hardware, and not for
lack of trying — two real, confirmed bugs were found and fixed along the
way:
1. the devicetree node backing the custom behavior had too long a name.
   `GLOBAL`/`EVENT_SOURCE` behaviors get relayed central-to-peripheral over
   BLE by looking up the target's device name in a fixed `char[16]` buffer
   (`zmk_split_transport_central_command.data.invoke_behavior.behavior_dev`,
   see `app/include/zmk/split/transport/types.h`), and a longer name gets
   silently truncated in transit and matches nothing on the peripheral —
   this is why stock `&soft_off`'s own node is named the cryptic `z_so_off`
   rather than something descriptive;
2. the peripheral-side handler for a relayed invocation
   (`split_svc_run_behavior` in `app/src/split/bluetooth/service.c`) is a
   Bluetooth GATT write callback — the BLE host stack's own thread, not a
   normal application thread — and the behavior was blocking that thread
   for the full flash sequence via `k_msleep()`.

Even after both fixes it still didn't fire reliably, most likely a dropped
or delayed packet somewhere in the relay itself with no way to confirm
further without a serial debug probe. [`src/totem_sleep.c`](src/totem_sleep.c)
now listens directly to `zmk_position_state_changed`, which
`physical_layouts.c` already raises locally on each device for its own
keys before anything is sent to the central at all — no relay, no GATT
callback, nothing that can drop a packet.

**The gesture triggers on release, not press, and runs on its own
workqueue.** Two more bugs surfaced even after moving to local listening,
both eventually traced to one root cause: `zmk_pm_soft_off()`
(`app/src/pm.c`) re-arms the kscan matrix as a wakeup source right before
powering off, and ZMK's own kscan matrix driver
(`app/module/drivers/kscan/kscan_gpio_matrix.c`) does its debounce/re-scan
polling on the default system workqueue.
- If the trigger keys were still held at the moment `zmk_pm_soft_off()`
  re-armed the wakeup source, that's a wake-detect line already active when
  it gets armed — a known nRF52 GPIO SENSE/LATCH hazard that can leave the
  wake mechanism stuck until a true power-on-reset. Fixed by only acting on
  the full press-then-release cycle: a pair latches "armed" the moment both
  its keys are simultaneously down, and the flash+sleep only fires once
  both have gone back up — by the time it runs, the keys are already
  confirmed released, so there's nothing to wait for or time out on.
- The flash sequence's `k_msleep()` calls used to run on the default system
  workqueue — the same one kscan's debounce/re-scan work uses.
  `k_msleep()` yields the CPU to *other threads* during that time, but
  doesn't let the *same* workqueue advance to its next queued item, so
  blocking it for hundreds of milliseconds starved kscan's own scan
  processing (for every key, not just the gesture ones) for as long as the
  block lasted — producing a burst of spurious repeated characters once the
  queue finally freed up, and blocking other typing while it ran. Fixed by
  running this code on `sleep_work_q`, a small dedicated workqueue with its
  own thread, so it can never again compete with kscan for the system
  workqueue.

Both fixes are independent of each other and both were necessary — trigger-
on-release alone still hit an unsafe timeout-based fallback if kscan was
starved and couldn't report the release in time; the dedicated queue alone
wouldn't have addressed the wakeup-source hazard.

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
- If you fork this rather than use it directly, update `url` in
  `config/boards/shields/totem/totem.zmk.yml` and `config/info.json` to
  point at your own repo, and swap the `west.yml`/build workflow remotes.

## Building

Push this to a GitHub repo and GitHub Actions (`.github/workflows/build.yml`)
will build all four images via ZMK's `build-user-config.yml` reusable
workflow — no local toolchain required. Flash `totem_left`/`totem_right` to
each half and `totem_dongle` to the receiver.
