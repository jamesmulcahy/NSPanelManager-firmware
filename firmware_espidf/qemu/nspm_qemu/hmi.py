"""Library: fetches the TFT's Nextion project (gui.HMI) from NSPanelManager, caches it under
qemu/build/hmi/, and parses it once per run. The fake display (fake_nextion.py) uses it to know
which pages, components and ids exist, and test_gui_data.py checks the firmware against it.

- by default, the HMI at the commit in qemu/nspanelmanager.ref (see upstream.py),
- NSPM_HMI=<path> uses a local file instead, e.g. an HMI that is not pushed yet,
- NSPM_HMI_REGION=eu tests against the EU layout (default: us).
"""

import os
from functools import lru_cache

from nextion_tools.hmi import Project

from . import upstream

HMI_PATH = "docker/HMI_files/tft_automation/{region}/gui.HMI"


def hmi_path() -> str:
    local = os.environ.get("NSPM_HMI")
    if local:
        return os.path.abspath(local)
    region = os.environ.get("NSPM_HMI_REGION", "us")
    cache = os.path.join(upstream.BUILD_DIR, "hmi", upstream.ref(), region)
    return upstream.fetch(HMI_PATH.format(region=region), cache, timeout=120)


@lru_cache(maxsize=None)
def load() -> Project:
    """The project, read once per test run."""
    return Project.load(hmi_path())
