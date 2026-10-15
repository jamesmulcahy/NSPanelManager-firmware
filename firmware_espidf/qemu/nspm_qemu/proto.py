"""Python bindings for the NSPanel protobufs, compiled on first import."""

import importlib
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROTO_DIR = os.path.join(os.path.dirname(_HERE), "proto")
_OUT_DIR = os.path.join(os.path.dirname(_HERE), "build", "pb")
_PROTOS = ["protobuf_nspanel.proto", "protobuf_nspanel_entity.proto"]


def _compile() -> None:
    from grpc_tools import protoc

    os.makedirs(_OUT_DIR, exist_ok=True)
    for proto in _PROTOS:
        src = os.path.join(_PROTO_DIR, proto)
        out = os.path.join(_OUT_DIR, proto.replace(".proto", "_pb2.py"))
        if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
            continue
        rc = protoc.main(["protoc", "-I" + _PROTO_DIR, "--python_out=" + _OUT_DIR, src])
        if rc != 0:
            raise RuntimeError("protoc failed on %s" % proto)


_compile()
if _OUT_DIR not in sys.path:
    sys.path.insert(0, _OUT_DIR)

nspanel = importlib.import_module("protobuf_nspanel_pb2")
entity = importlib.import_module("protobuf_nspanel_entity_pb2")
