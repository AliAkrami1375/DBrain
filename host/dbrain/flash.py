"""Writes a shard image into a board's artifact partition with esptool.

The partition offsets follow firmware/esp32/partitions*.csv and
firmware/esp8266/partitions.csv.
"""

import pathlib
import subprocess
import sys

from .artifact import parse

ARTIFACT_OFFSET = {
    "esp32": 0x1D0000, "esp32s2": 0x1D0000, "esp32s3": 0x1D0000, "esp32c3": 0x1D0000,
    "esp32c5": 0x1D0000, "esp32c6": 0x1D0000, "esp32c2": 0x170000, "esp8266": 0xF0000,
}


def flash_artifact(port: str, chip: str, image_path: pathlib.Path, baud: int = 460800,
                   esptool: str = "esptool.py", dry_run: bool = False) -> int:
    image = pathlib.Path(image_path).read_bytes()
    parse(image)   # refuse to flash a broken image
    if chip not in ARTIFACT_OFFSET:
        raise SystemExit("unknown chip %s" % chip)
    cmd = [esptool, "--chip", chip, "--port", port, "--baud", str(baud), "write_flash",
           hex(ARTIFACT_OFFSET[chip]), str(image_path)]
    print(" ".join(cmd), file=sys.stderr)
    if dry_run:
        return 0
    return subprocess.call(cmd)
