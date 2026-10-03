"""Validate actual ESPHome configurations in an isolated directory with dummy secrets."""

import base64
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def fixture(directory):
    directory = Path(directory)
    text = (ROOT / "example/novy-7831.yaml").read_text()
    text = text.replace("path: ../components", f"path: {ROOT / 'components'}")
    (directory / "novy.yaml").write_text(text)
    (directory / "secrets.yaml").write_text(
        "wifi_ssid: test-network\nwifi_password: test-password\n"
        "ota_password: test-only-not-for-deployment\napi_encryption_key: "
        + base64.b64encode(bytes(range(32))).decode() + "\n"
    )
    return text


def calibration():
    lines = ["fan_calibration:"]
    for speed in range(5):
        value = 2 + 30 * speed
        lines.extend([
            f"    - speed: {speed}", f"      min_power: {value - 1}", f"      max_power: {value + 1}",
        ])
    return "\n".join(lines + ["  light_threshold: 3"])


class ConfigTests(unittest.TestCase):
    def check(self, transform=lambda text: text, error=None, generate=False):
        with tempfile.TemporaryDirectory(prefix="novy-config-") as directory:
            text = transform(fixture(directory))
            path = Path(directory) / "novy.yaml"
            path.write_text(text)
            args = [sys.executable, "-m", "esphome", "compile" if generate else "config", str(path)]
            if generate:
                args.append("--only-generate")
            result = subprocess.run(args, capture_output=True, text=True)
            output = result.stdout + result.stderr
            if error:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(error, output)
            else:
                self.assertEqual(result.returncode, 0, output)

    def test_version(self):
        from esphome.const import __version__
        self.assertEqual(__version__, "2026.9.0")

    def test_commissioning_example(self):
        self.check()

    def test_captured_power_waveform_codegen(self):
        self.check(lambda t: t.replace("fan_calibration: []",
                   "power_waveform: [-12420, 370, -400, 720, -760, 370, -16220]\n  fan_calibration: []"),
                   generate=True)

    def test_invalid_power_waveform(self):
        for pulses, error in (("[-400, 0]", "non-zero"),
                              ("[-400, -720]", "alternate"),
                              ("[-70000, 370]", "value must be"),
                              ("[-400]", "at least")):
            with self.subTest(pulses=pulses):
                self.check(lambda t: t.replace("fan_calibration: []",
                           f"power_waveform: {pulses}\n  fan_calibration: []"), error)

    def test_full_calibration_and_axis_feedback_codegen(self):
        self.check(lambda t: t.replace("fan_calibration: []", calibration() +
                   "\n  fan_feedback_valid:\n    name: Fan feedback\n"
                   "  light_feedback_valid:\n    name: Light feedback"), generate=True)

    def test_meter_wiring(self):
        self.check(lambda t: t.replace("fan_channel: 2", "fan_channel: 1"), "must differ")
        self.check(lambda t: t.replace("update_interval: 500ms", "update_interval: 3s"),
                   "averaging_window must exceed update_interval")
        self.check(lambda t: t.replace("  tx_pin: ${jsy_tx_pin}\n", ""), "requires uart")

    def test_pairing_code_bounds(self):
        for code in ("0", "11"):
            with self.subTest(code=code):
                self.check(lambda t: t.replace('pairing_code: "1"', f'pairing_code: "{code}"'), "value must be")

    def test_partial_calibration(self):
        self.check(lambda t: t.replace("fan_calibration: []", "fan_calibration:\n    - speed: 0\n      min_power: 1\n      max_power: 2"), "all five unique")

    def test_duplicate_calibration(self):
        self.check(lambda t: t.replace("fan_calibration: []", calibration().replace("speed: 4", "speed: 3")), "all five unique")

    def test_invalid_ranges(self):
        for low, high, error in (("3", "1", "max_power"), (".nan", "3", "finite"), ("-1", "3", "non-negative"),
                                 ("1", "40", "must not overlap")):
            with self.subTest(low=low, high=high):
                self.check(lambda t: t.replace("fan_calibration: []", calibration().replace("min_power: 1\n      max_power: 3", f"min_power: {low}\n      max_power: {high}", 1)), error)

    def test_invalid_timing(self):
        self.check(lambda t: t.replace("step_timeout: 30s", "step_timeout: 9s"), "step_timeout must exceed")
        self.check(lambda t: t.replace("stale_timeout: 30s", "stale_timeout: 3s"), "stale_timeout must exceed")

    def test_transmitter_contract(self):
        self.check(lambda t: t.replace("non_blocking: true", "non_blocking: false"), "requires non_blocking")
        self.check(lambda t: t.replace("eot_level: false", "eot_level: true"), "requires non_blocking")
        self.check(lambda t: t.replace("carrier_duty_percent: 100%", "carrier_duty_percent: 50%"), "requires carrier_duty_percent")
        self.check(lambda t: t.replace("  eot_level: false", "  eot_level: false\n  on_complete:\n    - logger.log: test"), "dedicated Novy transmitter")

    def test_light_restore_rejected(self):
        self.check(lambda t: t.replace("restore_mode: ALWAYS_OFF", "restore_mode: ALWAYS_ON"), "restore_mode must be ALWAYS_OFF")


if __name__ == "__main__":
    unittest.main()
