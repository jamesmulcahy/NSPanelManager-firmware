"""Python bindings for the NSPanel protobufs.

The firmware repo only carries protobuf-c output (lib/ProtoBuf), so the .proto
sources come from NSPanelManager, which owns the schema:

- by default, fetched from GitHub at PROTO_REF and cached under build/,
- NSPM_PROTO_REF=<commit or branch> fetches another revision,
- NSPM_PROTO_DIR=<dir> uses a local checkout's docker/protobuf instead,
  e.g. to test a schema change before it is pushed.

PROTO_REF is pinned, so an upstream schema change cannot break these tests
until someone bumps it. A newer manager schema than the one lib/ProtoBuf was
generated from is fine: protobuf is wire-compatible, and the firmware ignores
fields it does not know.
"""

import hashlib
import importlib
import os
import sys
import urllib.request

# NSPManager/NSPanelManager devel, "Send NSPanels the POSIX TZ rule for the manager's timezone (#405)".
PROTO_REF = "253cb2755726f7083e25448efe9ba23e66f5e4d1"
PROTO_URL = "https://raw.githubusercontent.com/NSPManager/NSPanelManager/{ref}/docker/protobuf/{name}"
PROTOS = ["protobuf_nspanel.proto", "protobuf_nspanel_entity.proto"]

_BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build")


def _proto_dir() -> str:
    local = os.environ.get("NSPM_PROTO_DIR")
    if local:
        return os.path.abspath(local)
    ref = os.environ.get("NSPM_PROTO_REF", PROTO_REF)
    cache = os.path.join(_BUILD, "proto", ref)
    os.makedirs(cache, exist_ok=True)
    for name in PROTOS:
        path = os.path.join(cache, name)
        # A branch moves, so only the pinned commit is served from cache.
        if os.path.exists(path) and ref == PROTO_REF:
            continue
        with urllib.request.urlopen(PROTO_URL.format(ref=ref, name=name), timeout=30) as r:
            data = r.read()
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    return cache


def _compile(src_dir: str, out_dir: str) -> None:
    from grpc_tools import protoc

    os.makedirs(out_dir, exist_ok=True)
    for name in PROTOS:
        src = os.path.join(src_dir, name)
        out = os.path.join(out_dir, name.replace(".proto", "_pb2.py"))
        if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
            continue
        if protoc.main(["protoc", "-I" + src_dir, "--python_out=" + out_dir, src]) != 0:
            raise RuntimeError("protoc failed on %s" % src)


_src = _proto_dir()
_out = os.path.join(_BUILD, "pb", hashlib.sha1(_src.encode()).hexdigest()[:12])
_compile(_src, _out)
if _out not in sys.path:
    sys.path.insert(0, _out)

nspanel = importlib.import_module("protobuf_nspanel_pb2")
entity = importlib.import_module("protobuf_nspanel_entity_pb2")
