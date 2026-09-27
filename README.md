# Novy 7831 ESPHome controller

An ESP32 and STX882 transmitter control a Novy hood over 433.92 MHz RF.
Home Assistant supplies the smart plug's power readings; classification and
command sequencing run on the ESP32. A local ESPHome external component exposes
a standard four-speed fan and an on/off light, corrected by measured power.

**Status: software validated; physical Novy 7831 acceptance is still required.**
The installed receiver already works with its physical remote. ESP RF timing,
pairing, power signatures and command semantics must still be checked on it.

## Setup

1. Use Python 3.13 and install `pip install -r requirements.txt` in a virtual
   environment. Validation is pinned to **ESPHome 2026.9.0 / ESP-IDF 5.5.5**.
2. Copy `example/secrets.yaml.example` to `example/secrets.yaml`. Fill in Wi-Fi,
   a unique OTA password, and a native API encryption key generated with
   `openssl rand -base64 32`. The secrets file is ignored by Git.
3. Edit the substitutions in `example/novy-7831.yaml`: board, DATA GPIO,
   pairing code (1–10), and the **original smart plug power entity** in HA.
   Do not point it at the measured-power entity exported by this controller.
4. Validate and compile:

   ```sh
   esphome config example/novy-7831.yaml
   esphome compile example/novy-7831.yaml
   ```

5. Follow [commissioning](docs/commissioning.md) for wiring, first flash,
   raw commands, calibration and acceptance. No tool here flashes automatically.

The reference board is `esp32-c3-devkitm-1`, using GPIO4 for STX882 DATA.
This is a build profile, not confirmation of your board or wiring. Use ESP-IDF
and an ESP32 variant with RMT support. The external component source is local;
copy the `components` directory alongside the example when moving the config.

## Home Assistant entities

| Entity | Meaning |
| --- | --- |
| Fan | Off, speeds 1–3, boost (speed 4); HA percentages 25/50/75/100 |
| Light | On/off only; dimming and effects are unsupported |
| Measured power | Original incoming power, including invalid/unavailable values |
| Average power | Average of actual samples in each completed window |
| Reading age | Seconds since the last finite, non-negative received power value |
| Feedback valid | Whether the inferred state currently authorizes target commands |
| Inferred mode | `off`, `fan_1`…`fan_3`, `boost`, with `_light` if on; otherwise `unknown` |
| Command status | Ready, transmission/confirmation progress, or rejection/failure reason |
| Five raw buttons | Power, light, speed up, speed down, Novy; disabled by default in HA |

Add the node through HA's ESPHome integration. Enable raw buttons in each
entity's settings for commissioning. No HA automation or separate Novy Cooker
Hood integration is needed. Ordinary `fan.turn_on`, `fan.set_percentage`,
`fan.turn_off`, `light.turn_on` and `light.turn_off` requests use the ESP's
controller. A fan-on request without a speed uses the current speed if already
running, otherwise speed 1. Fan off walks down with confirmed minus commands.

Fan/light values represent the **last confirmed observation**. Native ESPHome
fan/light entities cannot independently express an unknown power-inference
state: before the first observation they display off, and during feedback loss
they retain previous values. Always consult **Feedback valid**, particularly in
HA automations. Neither the initial off value nor a retained value proves the
physical hood is off. Requests are refused when feedback is invalid.

The adapter intercepts normal binary light requests before ESPHome publishes
their target, retaining the observation until confirmation. Publishing an
observation never sends RF. Do not configure light effects, flashes or startup
automations to control this device.

## Calibration and feedback

The example ships with `calibration: []`: raw commands work, but fan/light
targets cannot transmit. Replace it with **all ten** speed/light combinations
after measurement. Each entry has `speed` (0–4), `light` (boolean), `min_power`
and `max_power` (inclusive watts). Duplicate combinations, partial tables,
negative/non-finite values and reversed ranges are rejected at config time.

Exactly one range must contain the averaged reading. Overlapping ranges are
allowed in the configuration, but readings in the overlap are **ambiguous** and
cannot authorize control. There is no nearest-match guess and no learned or
predicted state substituted for measurement.

| Setting | Default | Behavior |
| --- | --- | --- |
| `averaging_window` | `3s` | Average only samples actually received in the window |
| `stale_timeout` | `30s` | Invalidate after no valid received sample for this long |
| `settle_time` | `3s` | Ignore inference samples until this long after RF completes |
| `step_timeout` | `30s` | Stop waiting for confirmation; also bound missing RF completion |

Two consecutive populated windows must agree before a state is confirmed.
Empty windows, NaN, negative values, unmatched ranges and ambiguous ranges
invalidate feedback immediately. A first window indicating a different state
also prevents use of the old observation while the second window is pending.
The 30-second stale timeout is an additional guard, **not permission to reuse
the last reading in otherwise empty windows**.

Choose an averaging window long enough for the plug's real reporting cadence,
including jitter. With the default, the source needs updates more frequently
than once every three seconds, even when consumption is unchanged. If the
integration only emits state changes, state-aware control may remain invalid;
use genuine periodic meter reports or a different metering path. Do not add a
heartbeat that republishes cached power to make the validity indicator green.

HA remains a dependency. A received HA state can itself be cached; receipt age
does not prove the meter just measured it. Verify the plug integration marks
loss of communication unavailable and provides genuine periodic reports.
Disconnect detection also checks API state subscribers, but another subscribed
client can conceal HA's departure; sample/window expiration remains necessary.

## Commands and failure behavior

- An already-satisfied target sends nothing. Otherwise, the controller sends
  one fan step or one light toggle, waits for RF completion, discards settling
  samples and waits for two fresh confirming windows before proceeding.
- Targets are serialized. A second target is rejected while busy. No target,
  prediction or pending operation is restored after reboot or reconnection.
- Unchanged feedback waits until the deadline; an unexpected confirmed change
  aborts the target. Invalid feedback also aborts. There are no automatic RF
  retries. Command status reports the failure; a later recovery does not resume
  the abandoned target.
- A raw button cancels a pending target. If RF is already in flight, one raw
  press can wait until it finishes; further raw presses are rejected while that
  slot is occupied. A started waveform cannot be recalled.
- If the transmitter never completes, its lock stays held to prevent overlapping
  transmissions. Late completion clears the lock but reports failure. Inspect
  the device before manually retrying; no acknowledgement exists over RF.
- Boost expiry, delayed stop and physical-remote changes are reflected only
  when the measured signature settles. No autonomous follow-up is sent.

The RF transmitter must be dedicated to this component: no other actions,
RF proxy or `on_transmit`/`on_complete` automations may use it. Keep its DATA pin
non-inverted, duty at 100%, end level low, and nonblocking mode enabled.

## RF source and validation

Commands and channel prefixes follow
[renedis/ESP32_Novy_Controller](https://github.com/renedis/ESP32_Novy_Controller).
Its `setProtocol(12)` replaces the earlier 350 µs pulse setting with 320 µs;
[RC-Switch](https://github.com/sui77/rc-switch/blob/master/RCSwitch.cpp) also
defaults each `send()` to ten frames. Thus a button consists of two ten-frame
bursts for light and three for the other commands, separated by 50 ms low gaps.
Bits are sent MSB first: zero is low 320/high 640 µs, one low 640/high 320 µs,
and each frame ends with low 11520/high 320 µs synchronization. Output returns
low after the last frame. The STX882 generates the 433.92 MHz carrier; ESPHome
outputs the unmodulated envelope using RMT.

The implementation encodes the entire button press as one raw waveform, avoiding
differences in ESPHome's RC-Switch sync placement and per-action repeat defaults.
These source-derived defaults are not a substitute for testing on the hood.

```sh
bash tests/run_tests.sh
python -m unittest discover -s tests -p test_config.py
python tests/compile_example.py
```

The host suite exercises the production controller and encoder with address and
undefined-behavior sanitizers. Configuration tests use isolated dummy secrets;
the compile script builds the reference ESP32-C3 firmware in a temporary
directory and never uploads it. CI runs the same checks.
