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
    lines = ["calibration:"]
    for speed in range(5):
        for light in (False, True):
            value = 2 + 30 * speed + 10 * light
            lines.extend([
                f"    - speed: {speed}", f"      light: {str(light).lower()}",
                f"      min_power: {value - 1}", f"      max_power: {value + 1}",
            ])
    return "\n".join(lines)


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

    def test_full_calibration_codegen(self):
        self.check(lambda t: t.replace("calibration: []", calibration()), generate=True)

    def test_pairing_code_bounds(self):
        for code in ("0", "11"):
            with self.subTest(code=code):
                self.check(lambda t: t.replace('pairing_code: "1"', f'pairing_code: "{code}"'), "value must be")

    def test_partial_calibration(self):
        self.check(lambda t: t.replace("calibration: []", "calibration:\n    - speed: 0\n      light: false\n      min_power: 1\n      max_power: 2"), "all ten unique")

    def test_duplicate_calibration(self):
        self.check(lambda t: t.replace("calibration: []", calibration().replace("speed: 4", "speed: 3")), "all ten unique")

    def test_invalid_ranges(self):
        for low, high, error in (("3", "1", "max_power"), (".nan", "3", "finite"), ("-1", "3", "non-negative")):
            with self.subTest(low=low):
                self.check(lambda t: t.replace("calibration: []", calibration().replace("min_power: 1\n      max_power: 3", f"min_power: {low}\n      max_power: {high}", 1)), error)

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
