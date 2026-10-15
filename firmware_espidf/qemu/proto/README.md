Copied from NSPanelManager `docker/protobuf/` at commit ca00441a. These are the
schemas the manager speaks. They are a superset of what
`lib/ProtoBuf/*.pb-c.*` was generated from (they add media-player fields),
which is wire-compatible: the firmware ignores fields it does not know.

`nspm_qemu/proto.py` compiles them with grpcio-tools on first use.
