# Power command capture, 2026-10-01

The installed Novy 7831 initially responded to the STX882 light and speed
commands but ignored the built-in power command. Three brief power presses and three light
presses from the working original remote were captured on a 433 MHz SRX882S,
connected to GPIO27 on the classic AZDelivery ESP32. ESPHome was 2026.9.0.

Complete frames consistently decode to these pairing-code-1 payloads:

| Button | Bits |
| --- | --- |
| Power | `010101010111010011` |
| Light | `010101010111010001` |

Both match the built-in encoder's bits. The original remote's typical measured
DATA pulse pairs, in microseconds, differ from that encoder:

| Segment | Original remote at receiver DATA | ESPHome transmitter at receiver DATA |
| --- | --- | --- |
| Zero | low 398–400, high 719–722 | low about 320, high about 635 |
| One | low 754–762, high 368–372 | low about 635, high about 330 |
| Between frames | low about 16220, high about 370 | low about 11510, high about 330 |

The original power capture also includes an initial low gap near 12420 us.
The most complete brief presses contain five frames. Captures have occasional
noise-corrupted pulse pairs, so the candidate uses rounded representative
timings rather than replaying those glitches. These are receiver observations,
not logic-analyzer measurements of the remote's transmitter input.

`example/novy-power-waveform.yaml` contains that power-only candidate. The
optional `novy.power_waveform` setting supplies a whole press as signed
microseconds (positive high, negative low), using the existing command lock
and completion path. Without it all commands retain their built-in waveform;
with it light, plus, minus and Novy still retain theirs. The full supplied
waveform already includes pairing bits, so it is independent of `pairing_code`.

Use the updated local external component and merge this package into the
configuration to test it. On the dashboard the component files are staged under
`novy-power-capture-components/novy`; the API encryption key stays in its
existing secret. Nothing is transmitted automatically on boot or reception.

**Raw power was confirmed working by the user on 2026-10-01** after installing
the five-frame captured override. The change adjusts pulse widths, frame gaps
and repetition together; the test does not identify which difference was
essential. It validates this hood and pairing code, not other Novy models.
The final dashboard config keeps the captured override and removes temporary
receiver logging. Fan/light target controls still need power calibration.
