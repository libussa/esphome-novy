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

## 4. Enable and accept state-aware controls

Validate, compile and manually upload the configuration with the full table.
Confirm these scenarios; leave physical acceptance pending until they pass:

- Two populated stable windows establish the correct mode for every combination.
- Already-satisfied fan/light requests send no RF.
- All fan targets reach the requested speed, including off and boost, while
  preserving the light. Every intermediate step waits for feedback.
- Light requests toggle only when needed and preserve fan speed.
- Physical-remote and hood-panel changes correct the displayed observations.
- Boost expiry and any delayed stop are reported when they actually occur.
- An unexpected physical change during a target operation aborts it, without
  extra commands trying to fight the user.
- Raw buttons cancel target operations without interleaving RF transmissions.
- Stop HA, interrupt the plug connection, and observe an unavailable source:
  feedback becomes invalid, pending targets stop, and new targets send nothing.
- Restore connectivity: only new observations reestablish validity; abandoned
  targets never resume. Verify cached HA values do not masquerade as new meter
  readings. Test while power is stable as well as when it changes.
- Reboot the ESP with the hood both on and off. No RF is sent at startup and
  Feedback valid stays false until new readings confirm the state.

Until feedback is valid, fan/light displays are only last-known values (off at
startup). Include the validity entity wherever HA automations use those states.
Record command errors and raw-button results rather than repeatedly retrying an
uncertain toggle.
