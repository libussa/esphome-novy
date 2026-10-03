# Commissioning and hardware acceptance

Software compilation and host tests do not establish RF reception, electrical
wiring, power classification or command semantics on a physical Novy 7831.
Keep a record of the board, receiver/remote model, firmware version, pairing
code, JSY wiring and CT placement, and measured results below.

## 1. Wire and configure

- Confirm the installed receiver works with its physical remote.
- Use the **433.92 MHz STX882 transmitter**, not the SRX882 receiver. Follow its
  printed DATA/VCC/GND labels; the order reverses when viewed from the other side.
- Connect DATA to the configured ESP GPIO (GPIO4 in the reference profile), VCC
  to a suitable 3.3 V supply, and GND to the ESP ground. Power the transmitter
  from the supply rail, not a GPIO. Attach a suitable antenna to ANT.
- Connect the **JSY-MK-194G** TTL side to the ESP: VCC to 3.3 V, GND to GND,
  JSY TX to the ESP RX pin and JSY RX to the ESP TX pin (GPIO16/GPIO17 on the
  AZDelivery profile). Its measurement side is isolated from this TTL side.
- The JSY's UL/UN terminals sample the hood's mains voltage, and its CTs sit
  around conductors inside the hood. **Switch off the circuit breaker** before
  this work and keep the ESP, transmitter and low-voltage wiring separated from
  mains wiring.
- Put one CT around the light's switched conductor only and the other around
  the motor's common conductor, not a single speed tap. Never pass both live
  and neutral of the same load through one CT. Record which JSY channel
  carries which load and set `light_channel` and `fan_channel` to match.
  Orientation does not matter: the controller uses power magnitudes.
- An LED light of about 7 W draws only about 30 mA, near the bottom of the
  JSY's current range. If **Light power** stays near 0 W with the light on,
  loop its conductor several turns through the CT and set `light_threshold`
  from the multiplied reading.
- Set the actual board, GPIOs, JSY channels and credentials.
  Default pairing code 1 is only a starting point. Read the remote's code using
  its manual rather than repeatedly sending unknown commands to nearby devices.
- Validate and compile before manually uploading:

  ```sh
  esphome config example/novy-7831.yaml
  esphome compile example/novy-7831.yaml
  esphome upload example/novy-7831.yaml
  esphome logs example/novy-7831.yaml
  ```

Add the node to HA using its encryption key. Keep `fan_calibration: []`
initially. Enable the raw button entities manually in HA for the following
checks. Logs warn about invalid or missing JSY replies; **Reading age** should
stay at about one second or less.

## 2. Verify RF before target controls

Test deliberate single presses while watching the hood. Record each result:

| Check | Required observation | Result |
| --- | --- | --- |
| Raw light | One toggle; no unintended dimming or double toggle | Pending |
| Raw plus from off | Starts fan at speed 1 | Pending |
| Raw plus at 1, 2, 3 | Increments exactly one step | Pending |
| Raw minus at 4, 3, 2 | Decrements exactly one step | Pending |
| Raw minus at 1 | Stops the fan | Pending |
| Raw power | Record actual behavior, including any delayed stop | Pending |
| Raw Novy | Record actual behavior; do not assume immediate off | Pending |
| Reboot / HA reconnect | No RF button event | Pending |

Do not enable state-aware controls unless the plus/minus behavior matches the
table. Different receiver semantics require a controller change. If reception
is unreliable or light presses dim, inspect the RF envelope/repetition with a
receiver or logic analyzer and adjust the encoder only with measured evidence.

### Capture the original remote with an SRX882S

For the classic AZDelivery ESP32 profile, the optional package
[`example/novy-rf-capture.yaml`](../example/novy-rf-capture.yaml) enables raw
pulse logging and RCSwitch decoding without acting on received signals.

Use a **433 MHz SRX882S**. Power off the ESP before connecting its printed
pad labels as follows:

| SRX882S pad | ESP32 connection |
| --- | --- |
| VCC | 3.3 V |
| CS | 3.3 V |
| GND | GND |
| DATA | GPIO27 |
| ANT | 433 MHz antenna |

CS must be high: the SRX882S sleeps if it is left floating. Follow the pad
labels rather than an assumed left-to-right order. Supply it from 3.3 V so
its DATA output stays compatible with the ESP32. See the
[manufacturer's pin definitions](https://www.nicerf.com/ask-modules/superheterodyne-receiver-srx882s.html)
and [CS behavior](https://www.nicerf.com/news/superheterodyne-receiver-module-srx882s-srx882.html).
The existing STX882 DATA connection stays on GPIO4.

To enable this diagnostic package, place it beside the main classic ESP32
configuration and add (or merge with an existing `packages` section):

```yaml
packages:
  rf_capture: !include novy-rf-capture.yaml
```

Set the main configuration's `logger.level` to `DEBUG` as well if it already
declares a logger level, because main-config values override package values.

Validate, compile and manually upload the main configuration before capturing.
Open ESPHome logs, then briefly press the original remote's power button three
times, with two seconds between presses. Repeat for light and speed up, and
record which button each capture belongs to. Use a little distance between the
remote and receiver (about one metre is a useful starting point). Keep both
raw timings and decoded results; an absent RCSwitch decode does not mean the
raw capture is unusable. Compare with one ESPHome raw power transmission
after recording the original remote. Remove the diagnostic package when done.

## 3. Measure each channel

Use the physical controls or raw buttons to set each combination. Allow the
motor to settle, then record at least 30 seconds of **Fan power** and **Light
power** per state. Repeat under normal airflow and mains variation. Observe
boost before its automatic expiry. Check that the light state does not move
the fan channel and the fan speed does not move the light channel.

| Fan speed | Light | Fan W (min–max) | Light W (min–max) |
| --- | --- | --- | --- |
| 0 | Off | | |
| 0 | On | | |
| 1 | Off | | |
| 1 | On | | |
| 2 | Off | | |
| 2 | On | | |
| 3 | Off | | |
| 3 | On | | |
| 4 / boost | Off | | |
| 4 / boost | On | | |

Enter measured, inclusive fan ranges in `fan_calibration` and a light
threshold between the light channel's off and on readings. Each entry follows
this shape (the placeholders below deliberately are not numeric defaults):

```yaml
fan_calibration:
  - speed: 0
    min_power: REPLACE_WITH_MEASURED_LOWER_BOUND
    max_power: REPLACE_WITH_MEASURED_UPPER_BOUND
  # Add speeds 1-4 with their measured ranges.
light_threshold: REPLACE_WITH_THRESHOLD
```

Use a modest margin supported by repeated measurements. Fan ranges must not
overlap; if two speeds cannot be separated on the motor channel, improve the
measurement rather than widening ranges. Readings between ranges, such as
motor acceleration, are reported as unrecognized rather than guessed.

Keep `averaging_window` above `update_interval` so that each window holds
several responses. Keep `step_timeout` greater than
`settle_time + 2 * averaging_window`, with additional room for real motor
acceleration; the JSY itself needs about a second to report a stable value.

### Expected response time

Home Assistant only shows a state once the controller confirms it. The ESPHome
API then pushes it immediately. With the AZDelivery profile (500 ms polling,
1 s windows, 2 s settling), the delay comes from these steps:

- The JSY refreshes every 330 ms and needs about 1 s to settle on a new load.
- Two consecutive windows must agree, so about 2 s plus the partial window in
  which the change happened.
- After an RF command, samples are ignored until RF completes (about 0.6 s
  for light, 0.9 s for plus/minus) and `settle_time` has passed.
- A fan step also waits out motor acceleration, which reads as
  `unrecognized_power` until the motor reaches its range.

Estimates to check against the hardware:

| Change | Expected delay until HA updates |
| --- | --- |
| Light switched by remote or hood panel | about 2–3 s |
| Fan speed changed by remote or hood panel | about 3–5 s |
| Light target from HA | about 4 s (RF + settling + two windows) |
| Each fan step of an HA target | about 4–6 s |

The earlier Shelly-based setup took about 7–9 s per fan step and often could
not confirm the light at all. Light targets from HA used to appear after RF
alone because the state was assumed. They now wait for measurement, so they
show slightly later but are verified.

To measure the real delays, watch **Command status**, **Inferred mode**,
**Fan power** and **Light power** in the ESPHome logs or HA history. Time each
case above from the button press or HA call to the state change. Record the
motor's acceleration time per step: `step_timeout` must cover RF, settling,
acceleration and two windows with a margin.

If the readings are stable, shorten the delays in this order:

1. Reduce `averaging_window` (e.g. 700 ms with `update_interval: 300ms`). Keep
   at least two or three responses per window, and check that no window
   straddling a change confirms a wrong state.
2. Reduce `settle_time` only if the measured acceleration transient is reliably
   reported as `unrecognized_power` rather than a neighbouring speed. Settling
   applies to every RF command, including light toggles. The LED needs none,
   but there is currently no separate light setting.

Never shorten them by reusing cached readings or by skipping the two-window
agreement.

## 4. Enable and accept state-aware controls

Validate, compile and manually upload the configuration with the full table.
Confirm these scenarios; leave physical acceptance pending until they pass:

- Two populated stable windows establish each axis. Check Fan feedback valid and
  Light feedback valid separately; a fan transient must not block light targets.
- Already-satisfied fan/light requests send no RF.
- All fan targets reach the requested speed, including off and boost, while
  preserving the light. Every intermediate step waits for feedback.
- Light requests toggle only when needed and preserve fan speed.
- Physical-remote and hood-panel changes correct the displayed observations.
- Boost expiry and any delayed stop are reported when they actually occur.
- An unexpected fan speed during a fan target aborts it, without extra commands
  trying to fight the user. A separate light change does not block fan sequencing.
- Raw buttons cancel target operations without interleaving RF transmissions.
- Disconnect the JSY TX wire or its supply: feedback becomes invalid within one
  averaging window, pending targets stop, and new targets send nothing.
- Reconnect it: only new observations reestablish validity; abandoned targets
  never resume. Test while power is stable as well as when it changes.
- Reboot the ESP with the hood both on and off. No RF is sent at startup and
  Feedback valid stays false until new readings confirm the state.

Fan and light displays retain the last confirmed observation. Consult each
axis's validity entity wherever HA automations use its observed state; Feedback
valid means both are verified. Startup displays are off until observations
establish them, without startup RF. Record command errors and raw-button
results rather than repeatedly retrying an uncertain toggle.
