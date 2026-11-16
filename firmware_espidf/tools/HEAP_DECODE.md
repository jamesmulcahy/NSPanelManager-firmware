# Heap dumps and `heap_decode.py`

A firmware built with `-DNSPM_HEAP_DIAG` serves two dump endpoints on port 80. **A dump contains
the Wi-Fi PSK, the MQTT password and all of RAM. Never commit or share one;** keep them somewhere
untracked (`~/builds/claude-tmp/heap-metrics-scratch/`). The fleet build has neither endpoint.

```bash
# Build the diagnostic image (the env var adds the flag to every compile; do a clean build when
# switching, or PlatformIO may reuse objects):
cd firmware_espidf
PLATFORMIO_BUILD_FLAGS=-DNSPM_HEAP_DIAG pio run -e original_sonoff     # keep .pio/build/original_sonoff/firmware.elf

# Take a dump (text: one line per internal heap block; bin: all of DRAM, ~330 KB):
curl -s http://PANEL/debug/heap     -o heap.txt
curl -s http://PANEL/debug/heap.bin -o heap.bin

# Decode. The ELF must be the one the running image was built from (the dump carries its SHA-256
# prefix and the script refuses any other). Needs: pip install pyelftools
python3 tools/heap_decode.py heap.txt firmware.elf
python3 tools/heap_decode.py heap.bin firmware.elf --top 40 --core heap.core

# Typed look at a static in the snapshot (the local GDB has no Python; this needs none):
xtensa-esp32-elf-gdb -batch -ex 'p RoomManager::_home_page' firmware.elf heap.core
```

What the report shows:

- **Used blocks by class and size bucket.** Class is `movable` (malloc/new, internal: a lower
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` could move it), `pinned` (explicit internal caps) or
  `untracked` (the allocation table missed it).
- **Blocks by guessed type**: first word to a `_ZTV` vtable symbol (the C++ class), else a printable
  string, else `?`.
- **Heap bytes by owner** (`heap.bin` only): every static (`.data`/`.bss` object) and every task stack
  is a root; pointers are followed through the heap, treating any word that points into a block as a
  reference. Roots are listed greedily: each line counts the bytes no earlier line reached. The last line
  is what no root reaches (Wi-Fi and lwIP internals, pointers held only in registers or IRAM).
- `--core` writes an ELF core with one `PT_LOAD` for the snapshot, for GDB.

Limits: sizes are as requested for `movable`/`pinned` classes only via the metrics, the dump gives block
sizes (header and alignment included). A block is only as well typed as its first word. The `.bin` copy
holds the other core for tens of milliseconds, and the text walk holds the heap lock for about as long.

`/debug/heap` header lines carry `stale_entries`/`stale_bytes`: allocation-table entries no heap block
matches (a realloc that moved a block inside its heap is not reported by ESP-IDF), the error in the
`nspanel_heap_alloc_live_*` series.

Layout of `heap.bin`: a packed header (see `BinHeader` in `lib/Metrics/HeapDiag.cpp` and the `*_FMT`
constants in the script) followed by the DRAM bytes from `0x3FFAE000`.
