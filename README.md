# Novy 7831 ESPHome controller

An ESP32 and STX882 transmitter control a Novy hood over 433.92 MHz RF. A
JSY-MK-194G two-channel power meter (the JSY-MK-194T is register-compatible)
on the ESP32's UART measures the light and the fan motor separately;
classification and command sequencing run on the ESP32. A local ESPHome external component exposes a standard four-speed fan
and an on/off light, corrected by measured power.

**Status: raw RF confirmed on the installed AZDelivery board; the JSY meter is
not yet installed.** Its fan ranges are provisional, carried over from earlier
whole-hood plug measurements. New installations still require RF, pairing,
meter wiring and per-channel calibration checks.

## Setup

1. Use Python 3.13 and install `pip install -r requirements.txt` in a virtual
   environment. Validation is pinned to **ESPHome 2026.9.0 / ESP-IDF 5.5.5**.
2. Copy `example/secrets.yaml.example` to `example/secrets.yaml`. Fill in Wi-Fi,
   a unique OTA password, and a native API encryption key generated with
   `openssl rand -base64 32`. The secrets file is ignored by Git.
3. Edit the substitutions in `example/novy-7831.yaml`: board, DATA GPIO,
   pairing code (1–10) and the JSY UART pins. Set `light_channel` and
   `fan_channel` to the JSY channels whose CTs surround each conductor.
4. Validate and compile:

   ```sh
   esphome config example/novy-7831.yaml
   esphome compile example/novy-7831.yaml
   ```

5. Follow [commissioning](docs/commissioning.md) for wiring, first flash,
   raw commands, calibration and acceptance. No tool here flashes automatically.

The reference board is `esp32-c3-devkitm-1`, using GPIO4 for STX882 DATA and
GPIO5/GPIO6 for the JSY. This is a build profile, not confirmation of your
board or wiring. Use ESP-IDF and an ESP32 variant with RMT support. The
external component source is local; copy the `components` directory alongside
the example when moving the config.

For a separate Novy board using the **AZDelivery ESP32 NodeMCU Dev Kit C with
CP2102**, use [`example/novy-7831-azdelivery.yaml`](example/novy-7831-azdelivery.yaml).
It targets the classic ESP32-WROOM-32 with `board: esp32dev`, 4 MB flash and
ESP-IDF 5.5.5; it needs no PSRAM. Connect STX882 DATA to GPIO4, VCC to 3.3 V and
GND to GND. The CP2102 USB connector supplies power and serial flashing/logging.
The Eaton USB/NUT controller stays on its separate ESP32-S3 board.

This installation's profile reads the JSY-MK-194G at its factory settings
(address 1, 4800 baud, 8N1): JSY TX to GPIO16, JSY RX to GPIO17, VCC to 3.3 V.
CT1 surrounds the light conductor and CT2 the motor's common conductor.
API and OTA share a unique encryption key from
`!secret api_encryption_key`. After filling in the secrets file, validate and
compile with `esphome config example/novy-7831-azdelivery.yaml` and
`esphome compile example/novy-7831-azdelivery.yaml`. Add the new node to HA's
ESPHome integration for the fan, light and diagnostic entities.

The AZDelivery profile includes the captured power waveform confirmed working
on this hood, via `example/novy-power-waveform.yaml`. That waveform includes
pairing code 1; changing `pairing_code` does not rewrite its captured bits.
See [capture results](docs/power-rf-capture.md). Receiver logging is optional;
follow the receiver capture section in the commissioning guide to enable it.
It also includes provisional fan ranges and a light threshold in
[`novy-power-calibration.yaml`](example/novy-power-calibration.yaml), derived
from earlier whole-hood measurements. Re-measure both on the JSY channels.
This calibration is not a default for other hoods.

## Home Assistant entities

| Entity | Meaning |
| --- | --- |
| Fan | Off, speeds 1–3, boost (speed 4); HA percentages 25/50/75/100 |
| Light | On/off only; dimming and effects are unsupported |
| Fan power | Average fan-channel power in each completed window |
| Light power | Average light-channel power in each completed window |
| Reading age | Seconds since the last valid JSY response |
| Feedback valid | Both fan speed and light state are confirmed |
| Fan feedback valid | Fan speed is confirmed |
| Light feedback valid | Light state is confirmed |
| Inferred mode | `off`, `fan_1`…`fan_3`, `boost`, with `_light` if on; otherwise `unknown` |
| Command status | Ready, transmission/confirmation progress, or rejection/failure reason |
| Five raw buttons | Power, light, speed up, speed down, Novy; disabled by default in HA |

Add the node through HA's ESPHome integration. Enable raw buttons in each
entity's settings for commissioning. No HA automation or separate Novy Cooker
Hood integration is needed. Ordinary `fan.turn_on`, `fan.set_percentage`,
`fan.turn_off`, `light.turn_on` and `light.turn_off` requests use the ESP's
controller. A fan-on request without a speed uses the current speed if already
running, otherwise speed 1. Fan off walks down with confirmed minus commands.

Fan and light values represent the last confirmed observation. At startup the
displayed light defaults to off until observation establishes it; no startup
restoration transmits RF. Consult **Fan feedback valid** and **Light feedback
valid** for each axis; **Feedback valid** means both are confirmed. An
out-of-range fan reading, such as motor acceleration, leaves the light usable.

The adapter intercepts normal binary light requests before ESPHome publishes
their target, retaining the previous value until feedback confirms the change.
Publishing either value never sends RF. Do not configure light effects, flashes
or startup automations.

## Calibration and feedback

The generic C3 example ships with `fan_calibration: []`: raw commands work and
the power sensors report, but fan/light targets cannot transmit. Replace it with
**all five** fan speeds after measurement, and set `light_threshold`. Each fan
entry has `speed` (0–4), `min_power` and `max_power` (inclusive watts on the
fan channel). Duplicate speeds, partial tables, overlapping or reversed ranges
and negative/non-finite values are rejected at config time.

Each axis is classified from its own channel. The light is on when its window
average reaches `light_threshold`. The fan speed is the single range that
contains the fan average; outside every range the speed is unrecognized, never
guessed. The JSY reports power magnitudes, so CT orientation does not matter.

The JSY refreshes its measurements every 330 ms; one read takes about 170 ms
at 4800 baud. The installed AZDelivery profile polls every 500 ms and uses
1-second windows, 2-second settling and a 15-second step timeout. Hardware
timing after upload remains to be checked. The generic component defaults
below remain suitable starting points for a separately commissioned
installation.

| Setting | Default | Behavior |
| --- | --- | --- |
| `update_interval` | `500ms` | Read both JSY channels; must be shorter than the window |
| `averaging_window` | `3s` | Average only responses actually received in the window |
| `stale_timeout` | `30s` | Invalidate after no valid response for this long |
| `settle_time` | `3s` | Ignore inference samples until this long after RF completes |
| `step_timeout` | `30s` | Stop waiting for confirmation; also bound missing RF completion |

Two consecutive populated windows must agree before an axis is confirmed.
Replies with a bad CRC, address or length, and replies that do not arrive
before the next poll, are logged and supply no sample. An empty window
invalidates both axes immediately. Out-of-range fan readings clear only fan
confidence. A first window indicating a different state also prevents use of
the old observation while the second window is pending. The stale timeout is
an additional guard, **not permission to reuse the last reading in otherwise
empty windows**.

The JSY driver is a single Modbus RTU read of registers 0x0048–0x0055. Its
register map and scaling follow the JSY-MK-194G/194T manuals and
[MycilaJSY](https://github.com/mathieucarbou/MycilaJSY). That library is not
linked because it requires the Arduino framework and owns the serial port.

## Commands and failure behavior

- An already-satisfied target sends nothing. Fan requests send one step and
  wait for RF completion, settling and two fresh speed-confirming windows before
  proceeding. They preserve the light.
- Light requests send one toggle and wait for two fresh light-confirming
  windows. They preserve the fan speed and proceed during fan transients.
- In-flight targets are serialized. A second target is rejected while busy. No target,
  prediction or pending operation is restored after reboot or reconnection.
- Unchanged feedback waits until the deadline; an unexpected confirmed change
  aborts a target when it differs from the expected step. An independent light
  change does not abort fan sequencing. Meter loss (an empty window) also
  aborts; fresh out-of-range motor transients wait until the deadline without
  retrying. There are no automatic RF retries. Command status reports the
  failure; a later recovery does not resume the abandoned target.
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

The host suite exercises the production controller, JSY frame parser and
encoder with address and undefined-behavior sanitizers. Configuration tests
use isolated dummy secrets; the compile script builds the reference ESP32-C3
firmware in a temporary directory and never uploads it. CI runs the same
checks.
