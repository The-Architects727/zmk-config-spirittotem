# Totem (custom build)

Custom ZMK firmware for the [TOTEM](https://github.com/eigatech/zmk-config)
split keyboard (dongle variant) — same hardware and layout as the original,
plus per-half battery-charge LEDs on the dongle, an on-demand battery-level
check, and independent sleep management for each half with a visible warning
flash before it happens.

Built on Seeed XIAO nRF52840 BLE controllers (one in each half, one in the
dongle/receiver). No soldering beyond what the original TOTEM build already
needs, aside from two optional status LEDs on the dongle.

## Contents

- [Keymap](#keymap)
- [Features](#features)
- [Building the firmware](#building-the-firmware)
- [Flashing](#flashing)
- [First-time setup](#first-time-setup)
- [Customizing](#customizing)
- [Implementation notes](#implementation-notes)

## Keymap

Colemak-DH based, 5 layers. `...` means "transparent" (falls through to the
layer below).

### Base

```
  Q      W      F      P      B        J      L      U      Y      '
  A      R      S      T      G        M      N      E      I      O
 Esc     Z      X      C      D      V        K      H      ,      .      /      -
   Ctrl/Del  Nav/Tab    Space     Shift/Ent  Sym/Bspc   Fun/\
```

### Nav

*Hold the left thumb's `Nav/Tab` key.*

```
 ...    ...    ...    ...    ...      ...    Home    Up    PgUp   End
 Gui    Alt    Ctrl   ...    ...      Del    Left   Down  Right   PgDn
 ...    ...    ...    ...    ...    ...      Ins    ...    ...    ...    ...    ...
     ...       ...       ...         ...       ...       ...
```

### Sym

*Hold the right thumb's `Sym/Bspc` key.*

```
  1      2      3      4      5        6      7      8      9      0
  !      @      #      $      %        ^      &      *      (      )
Studio   (      [      {      <      `        ~      >      }      ]      )     ...
     ...       ...       ...         ...       ...       ...
```

### Fun

*Hold the right thumb's `Fun/\` key.*

```
  F1     F2     F3     F4     F5       F6     F7     F8     F9    F10
 ...    ...    ...    ...    ...      ...    ...    ...    F11    F12
BatChk  Boot   ...    ...    ...    ...      ...    ...    ...    ...   ToGame  ...
     ...       ...       ...         ...       ...       ...
```

`BatChk` blinks out each half's battery charge — see
[Features](#features). `Boot` reboots that half into its UF2 bootloader.
`ToGame` switches to the Game layer.

### Game

*Toggled from Fun's `ToGame` key; `ToBase` returns to Base.*

```
  T      Q      W      E      R        Y      U      I      O      P
  G      A      S      D      F        H      J      K      L      ;
 ...     B      Z      X      C      V        N      M      ,      .      /     ...
    Shift      Ctrl     Space       Enter      Bspc     ToBase
```

Standard QWERTY with the thumb cluster set up for typical games (Shift,
Ctrl, Space on the left; Enter, Backspace on the right) rather than
mod-taps, since holding a key briefly for its "tap" behavior is exactly the
kind of thing you don't want fighting with WASD-style movement.

The full keymap source is [`config/totem.keymap`](config/totem.keymap).

## Features

- **Battery-charge LEDs on the dongle.** Two small LEDs (wired to the
  dongle's D0/D1 pins) show each half's status at a glance: **off** (asleep
  or disconnected), **dim** (connected, battery fine), **bright** (connected,
  battery low, under ~3.7V).
- **On-demand battery check.** Hold Fun, tap the key labeled `BatChk` above,
  and the two LEDs blink out each half's charge in 10% steps (left first,
  then right) so you can check the exact level rather than just "low/fine."
- **Manual sleep gesture.** Press Esc + Z together, then let go, to sleep
  the left half; Slash + Minus for the right half. Each half sleeps
  independently, so you can put just one to sleep or both. The XIAO's
  built-in green LED flashes 3 times as a warning immediately before it
  actually powers down. Any keypress on that half wakes it back up.
- **Auto-sleep when the dongle is out of reach.** If a half can't reach the
  dongle for 5 minutes — for example, you turned the dongle off for the
  night — it flashes the same warning and puts itself to sleep on its own,
  so the battery isn't drained by an idle radio searching for a connection
  that isn't there.

## Building the firmware

No local toolchain needed — GitHub Actions does the build for you.

1. Fork or copy this repository to your own GitHub account.
2. Push to `main`. The workflow in
   [`.github/workflows/build.yml`](.github/workflows/build.yml) runs
   automatically and builds four firmware images:
   `totem_left`, `totem_right`, `totem_dongle`, and `settings_reset`.
3. Once the workflow finishes (a few minutes), open its run under the
   **Actions** tab and download the **firmware** artifact — a zip
   containing all four `.uf2` files.

## Flashing

Each XIAO nRF52840 needs its matching `.uf2` file:

| Board | Firmware file |
|---|---|
| Left half | `totem_left-xiao_ble__zmk-zmk.uf2` |
| Right half | `totem_right-xiao_ble__zmk-zmk.uf2` |
| Dongle / receiver | `totem_dongle-xiao_ble__zmk-zmk.uf2` |

To flash one:

1. Plug the board into USB and double-tap its reset button to drop it into
   UF2 bootloader mode — it will show up as a drive named something like
   `XIAO-SENSE` or `XIAO-BLE`.
2. Drag the matching `.uf2` file onto that drive. The board reboots on its
   own once the copy finishes.
3. Repeat for the other two boards.

If a half is already running this firmware, you can also put it into
bootloader mode from the keyboard itself: hold Fun and tap the key labeled
`Boot` in the [Fun layer](#fun) above, instead of reaching for the reset
button.

The fourth file, `settings_reset-xiao_ble__zmk-zmk.uf2`, is a separate,
special-purpose image — see [First-time setup](#first-time-setup) for when
you'd use it.

## First-time setup

- **Pairing order determines which LED is which.** The dongle assigns
  "left" and "right" based on whichever half it bonds to *first*, not by
  reading a label off the hardware. Flash and power on the **left** half
  before the right one the first time you pair, so its LED lands on D0 as
  expected.
- **If the LEDs end up swapped:** either flash `settings_reset` to both
  halves and re-pair them in the correct order, or swap the
  `TOTEM_LEFT_SOURCE`/`TOTEM_RIGHT_SOURCE` values near the top of
  [`src/battery_led.c`](src/battery_led.c) and rebuild.
- **LED wiring:** connect each LED's anode (through a current-limiting
  resistor) to the dongle's D0 (left) or D1 (right) pin, and its cathode to
  GND. If you wire it the other way around, flip `GPIO_ACTIVE_HIGH` to
  `GPIO_ACTIVE_LOW` in
  [`totem_dongle.overlay`](config/boards/shields/totem/totem_dongle.overlay).
  The dongle works fine with no LEDs wired up at all — that feature just
  won't do anything.

## Customizing

- **Keymap:** edit [`config/totem.keymap`](config/totem.keymap), commit, and
  push — GitHub Actions rebuilds automatically.
- **Battery threshold:** `TOTEM_BATTERY_CHARGED_PCT` in
  [`src/battery_led.c`](src/battery_led.c) sets the low-battery cutoff
  (default 34%, ≈3.7V on ZMK's own voltage curve).
- **Sleep gesture keys:** the four position numbers near the top of
  [`src/totem_sleep.c`](src/totem_sleep.c) (`SLEEP_GESTURE_POS_*`) pick
  which two keys per half trigger sleep, if you'd rather use different ones.
- **Auto-sleep timeout:** `TOTEM_AUTO_SLEEP_TIMEOUT_MS` in
  [`src/totem_sleep.c`](src/totem_sleep.c), default 5 minutes.
- **If you fork this:** update the `url` field in
  [`config/boards/shields/totem/totem.zmk.yml`](config/boards/shields/totem/totem.zmk.yml)
  and [`config/info.json`](config/info.json) to point at your own repo.

## Implementation notes

Details for anyone modifying this firmware, not needed just to build and
use it.

**How the dongle knows a half's battery level.** ZMK's split transport
carries a battery-percentage report from each peripheral over the same link
used for key events, every `CONFIG_ZMK_BATTERY_REPORT_INTERVAL` seconds
(10s here). [`src/battery_led.c`](src/battery_led.c) subscribes to that and
lights the low-battery LED below 34% — the percentage ZMK's own
voltage-divider driver computes for ~3.7V.

**How "connected" is determined.** ZMK doesn't expose a clean central-side
"is peripheral N connected" event, so this is mostly inferred from battery
report freshness: 6 missed reports (~60s) before assuming a half is asleep
or disconnected. There's a faster path too — ZMK's own BLE disconnect
handler (`split_central_disconnected` in
`app/src/split/bluetooth/central.c`) sends a synthetic 0% battery report
the instant a connection actually drops, for any reason. `battery_led.c`
treats a 0% report as an immediate disconnect signal rather than a real
low-battery reading (which would otherwise light the LED bright right as a
half goes to sleep — backwards from the OFF state it should show), so a
real disconnect usually shows up much faster than the 60s fallback.

**Why the sleep gesture isn't a ZMK combo.** An earlier version was: matched
centrally on the dongle, with the trigger relayed back to the peripheral
over BLE via a custom behavior. That relay path turned out to be unreliable
on this hardware — a devicetree node name that was too long for ZMK's
16-byte split-transport name buffer, then a blocking call inside a
Bluetooth GATT callback, and even after fixing both it still occasionally
missed. [`src/totem_sleep.c`](src/totem_sleep.c) instead listens directly
to the position events ZMK already raises locally on each half for its own
keys — no BLE round-trip involved in the trigger at all.

**Why it triggers on release, not press, on its own workqueue.**
`zmk_pm_soft_off()` re-arms the keyboard matrix as a wakeup source right
before powering off; if the trigger keys were still held at that instant,
that's a wake line already active the moment it gets armed — an nRF52
hardware hazard that can leave the wakeup mechanism stuck until a full
power-on reset. Only acting once both keys have been *released* avoids that
entirely. Separately, the LED-flash sequence runs on its own dedicated
workqueue rather than the shared system one, because ZMK's own keyboard
matrix scanning also uses that shared queue — blocking it, even briefly,
was starving normal key scanning and causing spurious repeated characters.

## Credits

Based on [eigatech/zmk-config](https://github.com/eigatech/zmk-config)'s
TOTEM branches. ZMK firmware: [zmkfirmware/zmk](https://github.com/zmkfirmware/zmk).
