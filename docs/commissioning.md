# Commissioning and hardware acceptance

Software compilation and host tests do not establish RF reception, electrical
wiring, power classification or command semantics on a physical Novy 7831.
Keep a record of the board, receiver/remote model, firmware version, pairing
code, plug model/integration and measured results below.

## 1. Wire and configure

- Confirm the installed receiver works with its physical remote.
- Use the **433.92 MHz STX882 transmitter**, not the SRX882 receiver. Follow its
  printed DATA/VCC/GND labels; the order reverses when viewed from the other side.
- Connect DATA to the configured ESP GPIO (GPIO4 in the reference profile), VCC
  to a suitable 3.3 V supply, and GND to the ESP ground. Power the transmitter
  from the supply rail, not a GPIO. Attach a suitable antenna to ANT.
- Keep the ESP and transmitter on an insulated low-voltage supply outside the
  hood's mains wiring. No changes to the installed receiver or mains circuit are
  needed for this configuration.
- Keep the smart plug relay permanently on. Do not control the hood by cutting
  its mains power; hide its relay control if useful.
- Set the actual board, GPIO, original plug power entity ID and credentials.
  Default pairing code 1 is only a starting point. Read the remote's code using
  its manual rather than repeatedly sending unknown commands to nearby devices.
- Validate and compile before manually uploading:

  ```sh
  esphome config example/novy-7831.yaml
  esphome compile example/novy-7831.yaml
  esphome upload example/novy-7831.yaml
  esphome logs example/novy-7831.yaml
  ```

Add the node to HA using its encryption key. Keep `calibration: []` initially.
Enable the raw button entities manually in HA for the following checks.

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

## 3. Measure power and reporting cadence

Use the physical controls or raw buttons to set each combination. Allow the
motor to settle, then record at least 30 seconds of steady samples per state.
Repeat under normal airflow and mains variation. Record typical, minimum and
maximum watts and the maximum gap between genuine reports. Observe boost
before its automatic expiry. Keep the light at the brightness used for v1.

| Fan speed | Light | Typical W | Minimum W | Maximum W | Maximum report gap |
| --- | --- | --- | --- | --- | --- |
| 0 | Off | | | | |
| 0 | On | | | | |
| 1 | Off | | | | |
| 1 | On | | | | |
| 2 | Off | | | | |
| 2 | On | | | | |
| 3 | Off | | | | |
| 3 | On | | | | |
| 4 / boost | Off | | | | |
| 4 / boost | On | | | | |

Enter measured, inclusive ranges in `calibration`. Each list entry follows this
shape (the placeholders below deliberately are not numeric defaults):

```yaml
calibration:
  - speed: 0
    light: false
    min_power: REPLACE_WITH_MEASURED_LOWER_BOUND
    max_power: REPLACE_WITH_MEASURED_UPPER_BOUND
  # Add the other nine speed/light combinations with their measured ranges.
```

Use a modest margin supported by repeated measurements, without covering other
states. If states overlap, the controller will correctly mark them ambiguous.
Widening ranges until every reading matches does not solve ambiguity. Improve
the measurement or accept that automatic targets cannot be used in those states.
Changing bulb brightness can invalidate this calibration even though v1 does
not send brightness commands.

Choose `averaging_window` above the observed maximum report gap plus jitter.
Keep `step_timeout` greater than `settle_time + 2 * averaging_window`, with
additional room for real settling and reporting delays; increase `stale_timeout`
if necessary. Defaults assume frequent reporting. An integration that suppresses
identical readings may need genuine periodic reporting enabled at its source.
The installed AZDelivery profile instead uses
[`novy-shelly-power.yaml`](../example/novy-shelly-power.yaml) for a fresh Shelly
HTTP status request every second. Set the actual status URL and expected MAC,
reserve the address in DHCP, and use an internal template `hood_power` sensor
with `update_interval: never`. The package checks identity, relay state, errors
and the power value. Failure supplies NaN, rather than a cached heartbeat.

## 4. Enable and accept state-aware controls

Validate, compile and manually upload the configuration with the full table.
Confirm these scenarios; leave physical acceptance pending until they pass:

- Two populated stable windows establish each independently distinguishable
  axis. Check Fan feedback valid and Light feedback valid separately; a same-speed
  light overlap must not block fan targets.
- Already-satisfied fan/light requests send no RF.
- All fan targets reach the requested speed, including off and boost, while
  preserving the light. Every intermediate step waits for feedback.
- Light requests toggle only when needed and preserve fan speed. With
  `light_on_ambiguity: last_known`, check that one changed explicit target sends
  one toggle, updates the remembered value after RF completion, and leaves Light
  feedback valid false until verified. Repeating that target must send no toggle.
- Physical-remote and hood-panel changes correct the displayed observations.
- Boost expiry and any delayed stop are reported when they actually occur.
- An unexpected fan speed during a fan target aborts it, without extra commands
  trying to fight the user. A separate light change does not block fan sequencing.
- Raw buttons cancel target operations without interleaving RF transmissions.
- Stop HA, interrupt the plug connection, and observe an unavailable source:
  feedback becomes invalid, pending targets stop, and new targets send nothing.
- Restore connectivity: only new observations reestablish validity; abandoned
  targets never resume. Verify cached HA values do not masquerade as new meter
  readings. Test while power is stable as well as when it changes.
- Reboot the ESP with the hood both on and off. No RF is sent at startup and
  Feedback valid stays false until new readings confirm the state.

Fan displays retain the last confirmed speed. With the AZDelivery remembered-light
fallback, light displays may be an explicitly requested, unconfirmed assumption
after RF completion. Consult each axis's validity entity wherever HA automations
use its observed state; the original full Feedback valid means both are verified.
Startup displays are off until observations establish them, without startup RF.
Record command errors and raw-button results rather than repeatedly retrying an
uncertain toggle.
