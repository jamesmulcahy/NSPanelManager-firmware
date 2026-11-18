"""Library: downloads files from NSPanelManager and caches them under qemu/build/.
hmi.py fetches the HMI from NSPManager/NSPanelManager at the commit in qemu/nspanelmanager.ref
(NSPM_NSPANELMANAGER_REF=<commit or branch> fetches another revision instead); proto.py fetches the
.proto files from the fork at the commit in qemu/protobuf.ref (NSPM_PROTO_REF overrides it).
"""

import os
import urllib.request

QEMU_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF_FILE = os.path.join(QEMU_DIR, "nspanelmanager.ref")
BUILD_DIR = os.path.join(QEMU_DIR, "build")
PROTO_REF_FILE = os.path.join(QEMU_DIR, "protobuf.ref")
RAW_URL = "https://raw.githubusercontent.com/{repo}/{ref}/{path}"
HMI_REPO = "NSPManager/NSPanelManager"
PROTO_REPO = "jamesmulcahy/NSPanelManager"


def _read_pin(path: str) -> str:
    with open(path) as f:
        lines = [line.strip() for line in f if line.strip() and not line.lstrip().startswith("#")]
    if len(lines) != 1:
        raise ValueError("%s must hold exactly one commit, found %r" % (path, lines))
    return lines[0]


def pinned_ref() -> str:
    """The commit in nspanelmanager.ref (its one line that is not a comment)."""
    return _read_pin(REF_FILE)


def proto_ref() -> str:
    """The commit in protobuf.ref, of the fork the .proto files come from."""
    return os.environ.get("NSPM_PROTO_REF") or _read_pin(PROTO_REF_FILE)


def ref() -> str:
    """The revision to fetch from: NSPM_NSPANELMANAGER_REF, else the pinned commit."""
    return os.environ.get("NSPM_NSPANELMANAGER_REF") or pinned_ref()


def fetch(path: str, cache_dir: str, timeout: float = 60, repo: str = HMI_REPO,
          revision: str = "", pinned: str = "") -> str:
    """`repo`'s `path` at `revision` (default ref()), saved as cache_dir/<basename>. Returns the
    local path.

    Only the pinned commit (default pinned_ref()) is served from cache: a branch moves, so it is
    fetched every time.
    """
    revision = revision or ref()
    pinned = pinned or pinned_ref()
    local = os.path.join(cache_dir, os.path.basename(path))
    if os.path.exists(local) and revision == pinned:
        return local
    os.makedirs(cache_dir, exist_ok=True)
    with urllib.request.urlopen(RAW_URL.format(repo=repo, ref=revision, path=path), timeout=timeout) as r:
        data = r.read()
    with open(local + ".tmp", "wb") as f:
        f.write(data)
    os.replace(local + ".tmp", local)
    return local
