"""Library: Python bindings for the NSPanel protobufs, which the fake manager and the tests use
to build the config and state they publish and to decode what the panel sends.

The firmware repo carries only protobuf-c output (lib/ProtoBuf), so the .proto sources come
from NSPanelManager, which owns the schema, and are compiled here with grpcio-tools:

- by default, at the commit in qemu/nspanelmanager.ref (see upstream.py), cached under build/proto/,
- NSPM_PROTO_DIR=<dir> uses a local checkout's docker/protobuf instead,
  e.g. to test a schema change before it is pushed.

A newer manager schema than the one lib/ProtoBuf was generated from is fine: protobuf is
wire-compatible, and the firmware ignores fields it does not know.
"""

import hashlib
import importlib
import os
import sys

from . import upstream

PROTOS = ["protobuf_nspanel.proto", "protobuf_nspanel_entity.proto"]

_BUILD = upstream.BUILD_DIR


def _proto_dir() -> str:
    local = os.environ.get("NSPM_PROTO_DIR")
    if local:
        return os.path.abspath(local)
    cache = os.path.join(_BUILD, "proto", upstream.ref())
    for name in PROTOS:
        upstream.fetch("docker/protobuf/" + name, cache, timeout=30)
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
