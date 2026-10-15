"""Build the flash and eFuse images QEMU boots from.

The flash image is what `esptool.py write_flash` would leave on a 4 MB part:
bootloader, partition table, OTA data, app, and a LittleFS partition built
from data/ plus a config.json, so the panel boots already configured.
"""

import csv
import json
import os
import shutil
import subprocess
import tempfile
from typing import Dict, Optional

FLASH_SIZE = 4 * 1024 * 1024
BOOTLOADER_OFFSET = 0x1000
PARTITION_TABLE_OFFSET = 0x8000

# idf.py qemu's default ESP32 eFuse block (tools/idf_py_actions/qemu_ext.py):
# chip revision 3, everything else zero. 124 bytes: BLK0 (7 words) + BLK1-3.
_DEFAULT_EFUSE = bytes.fromhex(
    "00000000000000000000000000800000000000000000100000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000"
)


def _parse_int(text: str) -> int:
    text = text.strip()
    if text.upper().endswith("K"):
        return int(text[:-1], 0) * 1024
    if text.upper().endswith("M"):
        return int(text[:-1], 0) * 1024 * 1024
    return int(text, 0)


def read_partitions(csv_path: str) -> Dict[str, Dict[str, int]]:
    parts = {}
    with open(csv_path) as f:
        for row in csv.reader(line for line in f if line.strip() and not line.lstrip().startswith("#")):
            name, _type, _subtype, offset, size = (c.strip() for c in row[:5])
            parts[name] = {"offset": _parse_int(offset), "size": _parse_int(size)}
    return parts


def find_mklittlefs() -> str:
    candidates = [
        os.environ.get("MKLITTLEFS"),
        shutil.which("mklittlefs"),
        os.path.expanduser("~/.platformio/packages/tool-mklittlefs/mklittlefs"),
    ]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    raise FileNotFoundError("mklittlefs not found; set MKLITTLEFS or run `pio run -e qemu -t buildfs` once to install it")


def build_littlefs(data_dir: str, files: Dict[str, bytes], size: int, out_path: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.join(tmp, "fs")
        shutil.copytree(data_dir, root)
        for name, content in files.items():
            dest = os.path.join(root, name)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(content)
        # Same page and block size PlatformIO's buildfs target uses.
        subprocess.run([find_mklittlefs(), "-c", root, "-s", str(size), "-p", "256", "-b", "4096", out_path],
                       check=True, stdout=subprocess.DEVNULL)


def build_flash_image(build_dir: str, project_dir: str, panel_config: Optional[dict], out_path: str) -> None:
    parts = read_partitions(os.path.join(project_dir, "partitions.csv"))
    image = bytearray(b"\xff" * FLASH_SIZE)

    def place(offset: int, path: str) -> None:
        with open(path, "rb") as f:
            data = f.read()
        image[offset:offset + len(data)] = data

    place(BOOTLOADER_OFFSET, os.path.join(build_dir, "bootloader.bin"))
    place(PARTITION_TABLE_OFFSET, os.path.join(build_dir, "partitions.bin"))
    ota_data = os.path.join(build_dir, "ota_data_initial.bin")
    if os.path.exists(ota_data):
        place(parts["otadata"]["offset"], ota_data)
    place(parts["ota_0"]["offset"], os.path.join(build_dir, "firmware.bin"))

    fs = parts["spiffs"]  # LittleFS lives in the partition named "spiffs" (lib/LittleFSManager)
    files = {}
    if panel_config is not None:
        files["config.json"] = json.dumps(panel_config, indent=2).encode()
    with tempfile.TemporaryDirectory() as tmp:
        fs_path = os.path.join(tmp, "littlefs.bin")
        build_littlefs(os.path.join(project_dir, "data"), files, fs["size"], fs_path)
        place(fs["offset"], fs_path)

    with open(out_path, "wb") as f:
        f.write(image)


def _crc8_mac(data: bytes) -> int:
    """esp_rom_efuse_mac_address_crc8: CRC-8/MAXIM (reflected poly 0x8C, init 0)."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8C if crc & 1 else crc >> 1
    return crc


def build_efuse_image(mac: str, out_path: str) -> None:
    """Default eFuses with MAC_FACTORY and its CRC burned in.

    MAC_FACTORY is BLK0 bits 32..79 with mac[0] in the highest byte
    (esp_efuse_table.c), and MAC_FACTORY_CRC is bits 80..87.
    """
    octets = bytes(int(x, 16) for x in mac.split(":"))
    if len(octets) != 6:
        raise ValueError("bad MAC %r" % mac)
    efuse = bytearray(_DEFAULT_EFUSE)
    efuse[4:10] = octets[::-1]
    efuse[10] = _crc8_mac(octets)
    with open(out_path, "wb") as f:
        f.write(efuse)
