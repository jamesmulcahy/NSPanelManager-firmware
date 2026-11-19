"""Heap and task metrics on /metrics, and (diagnostic build only) the heap dump endpoints.

The first two tests run on any build. The rest need firmware built with -DNSPM_HEAP_DIAG and are
skipped otherwise:

    PLATFORMIO_BUILD_FLAGS=-DNSPM_HEAP_DIAG qemu/run_tests.sh -k heap

The dumps contain secrets (here, the QEMU panel's own); they are written under the test's log
directory, which is not committed.
"""

import os
import re
import subprocess
import sys
import time

import pytest

SERIES = re.compile(r"^([a-z_0-9]+)(\{[^}]*\})? (-?[0-9.e+-]+)$", re.M)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DECODER = os.path.join(ROOT, "tools", "heap_decode.py")


def scrape(rig):
    status, body = rig.http_get("/metrics")
    assert status == 200
    text = body.decode()
    values = {}
    for name, labels, value in SERIES.findall(text):
        assert (name, labels) not in values, "duplicate series %s%s" % (name, labels)
        values[(name, labels)] = float(value)
    return text, values


def value(values, name, labels=""):
    return values[(name, labels)]


def test_task_stacks_and_heap_window(booted):
    text, first = scrape(booted)
    assert "# HELP nspanel_task_stack_unused_bytes" in text

    # Our own tasks, and ESP-IDF's, each with the pool its stack is in.
    tasks = {m.group(1): m.group(2) for m in re.finditer(r'nspanel_task_stack_unused_bytes\{task="([^"]+)",mem="(\w+)"\} \d+', text)}
    assert "httpd" in tasks and "IDLE0" in tasks, sorted(tasks)
    assert set(tasks.values()) <= {"internal", "psram"}
    assert value(first, "nspanel_tasks") >= len(tasks) > 10
    httpd = value(first, "nspanel_task_stack_unused_bytes", '{task="httpd",mem="internal"}')
    assert 0 < httpd < 12288  # in bytes, so a 12 KB stack cannot have more unused than that
    assert value(first, "nspanel_task_scan_seconds") < 1

    # The window since boot is no higher than the free heap, and the since-boot minimum never rises
    # above any window (the first scrape's window IS since boot).
    window = value(first, "nspanel_heap_window_min_free_bytes", '{pool="internal"}')
    boot_min = value(first, "nspanel_heap_min_free_bytes", '{pool="internal"}')
    assert 0 < boot_min <= window <= value(first, "nspanel_heap_size_bytes", '{pool="internal"}')

    _, second = scrape(booted)
    window2 = value(second, "nspanel_heap_window_min_free_bytes", '{pool="internal"}')
    boot_min2 = value(second, "nspanel_heap_min_free_bytes", '{pool="internal"}')
    assert boot_min2 <= boot_min  # a minimum only goes down
    assert boot_min2 <= window2
    # The second window cannot be lower than the since-boot minimum, and is well above zero.
    assert window2 >= boot_min2 > 0


def test_mqtt_subscription_metrics(booted):
    """The subscriptions core's stats are on /metrics, and plausible after boot."""
    _, v = scrape(booted)
    assert value(v, "nspanel_mqtt_subscriptions", '{state="active"}') > 0
    assert value(v, "nspanel_mqtt_subscriptions", '{state="lingering"}') >= 0
    assert value(v, "nspanel_mqtt_subscription_holders") > 0
    assert value(v, "nspanel_mqtt_cache_entries") > 0
    assert value(v, "nspanel_mqtt_cache_bytes") > 0
    assert value(v, "nspanel_mqtt_messages_received_total") > 0
    # "active" includes filters still waiting for a slot among the SUBSCRIBEs in flight, so right
    # after boot it can briefly run ahead of the SUBSCRIBEs sent. Once the queue drains it cannot.
    deadline = time.monotonic() + 10
    while (value(v, "nspanel_mqtt_subscribe_requests_total", '{op="subscribe"}') <
           value(v, "nspanel_mqtt_subscriptions", '{state="active"}')) and time.monotonic() < deadline:
        time.sleep(0.5)
        _, v = scrape(booted)
    assert value(v, "nspanel_mqtt_subscribe_requests_total", '{op="subscribe"}') >= value(v, "nspanel_mqtt_subscriptions", '{state="active"}')
    for name in ("nspanel_mqtt_deliveries_pending", ):
        assert value(v, name, '{kind="state"}') >= 0 and value(v, name, '{kind="command"}') >= 0
    for reason in ("oversize", "fragment", "command_queue_full"):
        assert value(v, "nspanel_mqtt_messages_dropped_total", '{reason="%s"}' % reason) == 0
    for name in ("subscribe_retries", "subscribe_timeouts"):
        assert value(v, "nspanel_mqtt_%s_total" % name, '{op="subscribe"}') == 0
        assert value(v, "nspanel_mqtt_%s_total" % name, '{op="unsubscribe"}') == 0
    assert value(v, "nspanel_mqtt_state_coalesced_total") >= 0
    assert value(v, "nspanel_mqtt_replays_skipped_total") >= 0


def diag_scrape(booted):
    text, values = scrape(booted)
    if ("nspanel_heap_hook_active", "") not in values:
        pytest.skip("firmware built without -DNSPM_HEAP_DIAG")
    return text, values


def test_allocation_accounting(booted):
    text, values = diag_scrape(booted)
    assert value(values, "nspanel_heap_hook_active") == 1, "no PSRAM table: is PSRAM emulated?"
    assert value(values, "nspanel_heap_hook_table_overflow_total") == 0

    live = {}
    for (name, labels), v in values.items():
        m = re.match(r'\{class="(\w+)",le="([^"]+)"\}', labels)
        if m and name == "nspanel_heap_alloc_live_bytes":
            live[(m.group(1), m.group(2))] = v
    movable = sum(v for (c, _), v in live.items() if c == "movable")
    pinned = sum(v for (c, _), v in live.items() if c == "pinned")
    assert movable > 10000 and pinned > 10000, (movable, pinned)

    # What the tracking says is live, against what the allocator says is used. Not equal (headers,
    # alignment, allocations before the table existed), but the same order of magnitude.
    used = value(values, "nspanel_heap_size_bytes", '{pool="internal"}') - value(values, "nspanel_heap_free_bytes", '{pool="internal"}')
    assert 0.3 * used < movable + pinned < 1.3 * used, (movable, pinned, used)

    # Frees the table did not know are a small share of all of them.
    assert value(values, "nspanel_heap_hook_unknown_free_total") < 0.2 * sum(
        v for (n, _), v in values.items() if n == "nspanel_heap_alloc_total")

    # Churn goes up between scrapes, and the movable-at-low series are consistent: a higher threshold
    # leaves less above it.
    booted.http_get("/status_data")
    later_text, later = scrape(booted)
    with open(os.path.join(booted.panel.workdir, "metrics.txt"), "w") as f:
        f.write(later_text)
    total = lambda vs: sum(v for (n, _), v in vs.items() if n == "nspanel_heap_alloc_total")
    assert total(later) > total(values)
    series = [value(later, "nspanel_heap_movable_at_low_bytes", '{threshold="%d",window="boot"}' % t)
              for t in (64, 128, 256, 512, 1024, 2048, 4096, 8192)]
    assert series == sorted(series, reverse=True)
    assert value(later, "nspanel_heap_free_at_low_bytes", '{window="boot"}') >= value(
        later, "nspanel_heap_min_free_bytes", '{pool="internal"}')


def fetch(rig, path, dest):
    status, body = rig.http_get(path)
    assert status == 200
    with open(dest, "wb") as f:
        f.write(body)
    return body


def test_heap_dump_and_decoder(booted):
    diag_scrape(booted)
    build_dir = os.environ.get("NSPM_QEMU_BUILD_DIR") or os.path.join(ROOT, ".pio", "build", "qemu")
    elf = os.path.join(build_dir, "firmware.elf")
    if not os.path.exists(elf):
        pytest.skip("no firmware.elf in %s" % build_dir)
    try:
        import elftools  # noqa: F401
    except ImportError:
        pytest.skip("pyelftools not installed")

    out = booted.panel.workdir
    text = fetch(booted, "/debug/heap", os.path.join(out, "heap.txt")).decode()
    binary = fetch(booted, "/debug/heap.bin", os.path.join(out, "heap.bin"))

    assert text.startswith("# nspm heap dump v1")
    blocks = [l.split() for l in text.splitlines() if l and not l.startswith("#")]
    assert len(blocks) > 200
    used = [b for b in blocks if b[2] == "U"]
    assert sum(1 for b in used if b[3] in ("movable", "pinned")) > 0.8 * len(used)

    assert binary[:8] == b"NSPMHEAP" and len(binary) > 0x52000

    for dump, extra in (("heap.txt", []), ("heap.bin", ["--core", os.path.join(out, "heap.core")])):
        r = subprocess.run([sys.executable, "-I", DECODER, os.path.join(out, dump), elf] + extra, capture_output=True, text=True, timeout=300)
        assert r.returncode == 0, r.stderr
        assert "Used blocks by class and size bucket" in r.stdout
        assert "C++ " in r.stdout, "no vtable-typed blocks found:\n" + r.stdout
        if dump == "heap.bin":
            assert "Heap bytes by owner" in r.stdout
            with open(os.path.join(out, "decoded-bin.txt"), "w") as f:
                f.write(r.stdout)
        else:
            with open(os.path.join(out, "decoded-txt.txt"), "w") as f:
                f.write(r.stdout)

    # The wrong ELF is refused: flip a byte of a copy.
    bad = os.path.join(out, "wrong.elf")
    with open(elf, "rb") as src, open(bad, "wb") as dst:
        data = bytearray(src.read())
        data[-1] ^= 0xFF
        dst.write(data)
    r = subprocess.run([sys.executable, "-I", DECODER, os.path.join(out, "heap.txt"), bad], capture_output=True, text=True, timeout=300)
    assert r.returncode != 0 and "ELF mismatch" in r.stderr
