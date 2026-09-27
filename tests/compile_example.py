"""Compile the reference board using temporary test secrets; never flash it."""
import subprocess
import sys
import tempfile
from pathlib import Path

from test_config import fixture

with tempfile.TemporaryDirectory(prefix="novy-build-") as directory:
    fixture(directory)
    subprocess.run([sys.executable, "-m", "esphome", "compile", str(Path(directory) / "novy.yaml")], check=True)
