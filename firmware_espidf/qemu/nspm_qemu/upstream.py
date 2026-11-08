"""Library: downloads files from NSPanelManager at the commit pinned in qemu/nspanelmanager.ref,
and caches them under qemu/build/. hmi.py and proto.py fetch through it.

NSPM_NSPANELMANAGER_REF=<commit or branch> fetches from another revision instead.
"""

import os
import urllib.request

QEMU_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF_FILE = os.path.join(QEMU_DIR, "nspanelmanager.ref")
BUILD_DIR = os.path.join(QEMU_DIR, "build")
RAW_URL = "https://raw.githubusercontent.com/NSPManager/NSPanelManager/{ref}/{path}"


def pinned_ref() -> str:
    """The commit in nspanelmanager.ref (its one line that is not a comment)."""
    with open(REF_FILE) as f:
        lines = [line.strip() for line in f if line.strip() and not line.lstrip().startswith("#")]
    if len(lines) != 1:
        raise ValueError("%s must hold exactly one commit, found %r" % (REF_FILE, lines))
    return lines[0]


def ref() -> str:
    """The revision to fetch from: NSPM_NSPANELMANAGER_REF, else the pinned commit."""
    return os.environ.get("NSPM_NSPANELMANAGER_REF") or pinned_ref()


def fetch(path: str, cache_dir: str, timeout: float = 60) -> str:
    """NSPanelManager's `path` at ref(), saved as cache_dir/<basename>. Returns the local path.

    Only the pinned commit is served from cache: a branch moves, so it is fetched every time.
    """
    revision = ref()
    local = os.path.join(cache_dir, os.path.basename(path))
    if os.path.exists(local) and revision == pinned_ref():
        return local
    os.makedirs(cache_dir, exist_ok=True)
    with urllib.request.urlopen(RAW_URL.format(ref=revision, path=path), timeout=timeout) as r:
        data = r.read()
    with open(local + ".tmp", "wb") as f:
        f.write(data)
    os.replace(local + ".tmp", local)
    return local
