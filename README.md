# Novy 7831 ESPHome controller

An ESP32 and STX882 transmitter control a Novy hood over 433.92 MHz RF.
The smart plug supplies power readings, through Home Assistant or direct HTTP
polling; classification and command sequencing run on the ESP32. A local ESPHome external component exposes
a standard four-speed fan and an on/off light, corrected by measured power.

**Status: raw RF confirmed on the installed AZDelivery board; state-aware
control acceptance remains pending.** Its power signatures have been measured,
including warm-motor repeats. New installations still require RF, pairing,
command and power-feedback checks.

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

For a separate Novy board using the **AZDelivery ESP32 NodeMCU Dev Kit C with
CP2102**, use [`example/novy-7831-azdelivery.yaml`](example/novy-7831-azdelivery.yaml).
It targets the classic ESP32-WROOM-32 with `board: esp32dev`, 4 MB flash and
ESP-IDF 5.5.5; it needs no PSRAM. Connect STX882 DATA to GPIO4, VCC to 3.3 V and
GND to GND. The CP2102 USB connector supplies power and serial flashing/logging.
The Eaton USB/NUT controller stays on its separate ESP32-S3 board.

This installation's profile uses the local Novy component and polls the Shelly
Plug M Gen3 once per second, including when watts stay unchanged. Configure its
status URL and MAC address in the substitutions and reserve its IP in DHCP.
API and OTA share a unique encryption key from
`!secret api_encryption_key`. After filling in the secrets file, validate and
compile with `esphome config example/novy-7831-azdelivery.yaml` and
`esphome compile example/novy-7831-azdelivery.yaml`. Add the new node to HA's
ESPHome integration for the fan, light and diagnostic entities. The original
`sensor.prise_hotte_power` remains available separately in HA.

The AZDelivery profile includes the captured power waveform confirmed working
on this hood, via `example/novy-power-waveform.yaml`. That waveform includes
pairing code 1; changing `pairing_code` does not rewrite its captured bits.
See [capture results](docs/power-rf-capture.md). Receiver logging is optional;
follow the receiver capture section in the commissioning guide to enable it.
It also includes this hood's measured ten-state calibration and the initial and
warm-motor evidence in [power learning](docs/power-learning.md). The light-on
and light-off signatures overlap at some wattages. Fan targets still use the
independently confirmed speed; light targets use the remembered-state fallback
and remain unconfirmed until distinguishable feedback arrives. This calibration is not a default for other hoods.

## Home Assistant entities

| Entity | Meaning |
| --- | --- |
| Fan | Off, speeds 1–3, boost (speed 4); HA percentages 25/50/75/100 |
| Light | On/off only; dimming and effects are unsupported |
| Measured power | Original incoming power, including invalid/unavailable values |
| Average power | Average of actual samples in each completed window |
| Reading age | Seconds since the last finite, non-negative received power value |
| Feedback valid | Both fan speed and light state are confirmed |
| Fan feedback valid | Fan speed is confirmed independently of light ambiguity |
| Light feedback valid | Light state is confirmed rather than remembered/assumed |
| Inferred mode | `off`, `fan_1`…`fan_3`, `boost`, with `_light` if on; otherwise `unknown` |
| Command status | Ready, transmission/confirmation progress, or rejection/failure reason |
| Five raw buttons | Power, light, speed up, speed down, Novy; disabled by default in HA |

Add the node through HA's ESPHome integration. Enable raw buttons in each
entity's settings for commissioning. No HA automation or separate Novy Cooker
Hood integration is needed. Ordinary `fan.turn_on`, `fan.set_percentage`,
`fan.turn_off`, `light.turn_on` and `light.turn_off` requests use the ESP's
controller. A fan-on request without a speed uses the current speed if already
running, otherwise speed 1. Fan off walks down with confirmed minus commands.

Fan values represent the last confirmed speed. The AZDelivery light uses
`light_on_ambiguity: last_known`: an explicit changed on/off target sends one
toggle based on the remembered value and updates that value when RF completes.
This is an **unconfirmed assumption**, not acknowledgement from the hood. Equal
requests send no extra toggle. Distinguishable feedback corrects the remembered
value; stale, invalid or unrecognized feedback does not permit a fallback.
At startup the displayed light defaults to off until observation establishes it;
no startup restoration transmits RF.

Consult **Fan feedback valid** for fan observations and **Light feedback valid**
for verified light observations. The original **Feedback valid** means both are
confirmed. It can be false while fan targets remain usable. With the default
`light_on_ambiguity: reject`, only confirmed light state authorizes a light target.

The adapter intercepts normal binary light requests before ESPHome publishes
their target, retaining the previous value until feedback or an explicitly
enabled remembered light request finishes RF. Publishing either value never
sends RF. Do not configure light effects, flashes or startup automations.

## Calibration and feedback

The generic C3 example ships with `calibration: []`: raw commands work, but fan/light
targets cannot transmit. Replace it with **all ten** speed/light combinations
after measurement. Each entry has `speed` (0–4), `light` (boolean), `min_power`
and `max_power` (inclusive watts). Duplicate combinations, partial tables,
negative/non-finite values and reversed ranges are rejected at config time.

Each axis is classified independently. All matching ranges must agree on fan
speed to confirm speed, and must agree on light state to confirm light. Thus a
same-speed light overlap leaves the fan usable while light confidence is false.
Cross-speed overlap blocks fan targets. No nearest-range guess is used.
`light_on_ambiguity` accepts `reject` (default) or `last_known` (opt-in); the latter
allows a manual light target using the remembered value when fan feedback is
fresh and confirmed. It never makes assumed light state verified feedback.

The installed AZDelivery profile uses 2-second windows, 2-second settling and a
15-second step timeout with 1 Hz direct Shelly readings. Replaying the captured
transitions confirmed fan speed in about 7–9 seconds, including simulated RF,
without incorrect unique classifications. Hardware timing after upload remains
to be checked. The generic component defaults below remain suitable starting
points for a separately commissioned installation.

| Setting | Default | Behavior |
| --- | --- | --- |
| `averaging_window` | `3s` | Average only samples actually received in the window |
| `stale_timeout` | `30s` | Invalidate after no valid received sample for this long |
| `settle_time` | `3s` | Ignore inference samples until this long after RF completes |
| `step_timeout` | `30s` | Stop waiting for confirmation; also bound missing RF completion |

Two consecutive populated windows must agree before a state is confirmed.
Empty windows, NaN and negative values invalidate both axes immediately.
Unmatched ranges clear confidence; matching ranges that disagree clear only the
affected axis. A first window indicating a different state
also prevents use of the old observation while the second window is pending.
The 30-second stale timeout is an additional guard, **not permission to reuse
the last reading in otherwise empty windows**.

Choose an averaging window long enough for the plug's real reporting cadence,
including jitter. With the default, the source needs updates more frequently
than once every three seconds, even when consumption is unchanged. If the
integration only emits state changes, state-aware control may remain invalid;
use genuine periodic meter reports or a different metering path. Do not add a
heartbeat that republishes cached power to make the validity indicator green.

With the generic HA input, a received state can itself be cached; receipt age
does not prove the meter just measured it. Verify the plug integration marks
loss of communication unavailable and provides genuine periodic reports.
Disconnect detection also checks API state subscribers, but another subscribed
client can conceal HA's departure; sample/window expiration remains necessary.
The AZDelivery HTTP package reads the plug directly and checks its MAC, relay
state, errors and power value on each response. Failed requests or invalid
responses publish NaN; it never repeats a cached reading. An HA disconnection
still cancels a pending operation, while fresh plug samples can reestablish
feedback independently.

## Commands and failure behavior

- An already-satisfied target sends nothing. Fan requests send one step and
  wait for RF completion, settling and two fresh speed-confirming windows before
  proceeding. They preserve the light regardless of light inference ambiguity.
- With `last_known`, a changed light target sends one toggle, records its assumed
  result after RF completion, and releases the operation. Light feedback remains
  false until fresh unambiguous observations verify or correct that assumption.
- In-flight targets are serialized. A second target is rejected while busy. No target,
  prediction or pending operation is restored after reboot or reconnection.
- Unchanged feedback waits until the deadline; an unexpected confirmed change
  aborts a fan target when its speed differs from the expected step. An independent
  light change does not abort fan sequencing. Meter loss also aborts; fresh
  out-of-range motor transients wait until the deadline without retrying. There are no automatic RF
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
