"""Boot, registration and the steady state a configured panel settles into."""

import json
import urllib.error
import urllib.request

import pytest

from nspm_qemu.fake_manager import QEMU_HOST_ADDRESS
from nspm_qemu.proto import nspanel as pb
from nspm_qemu.rig import BOOT_TIMEOUT
from shared_behaviour import lvgl_only, nextion_only


def test_registers_with_manager(rig):
    rig.start()
    request = rig.manager.wait_for_registration(timeout=BOOT_TIMEOUT)

    assert request["mac_origin"] == rig.mac
    assert request["friendly_name"] == rig.hostname
    assert request["model"] == "sonoff"
    assert request["address"] == "10.0.2.15"  # QEMU user-mode DHCP lease, via the Ethernet path


@nextion_only
def test_receives_config_and_shows_screensaver(booted):
    booted.panel.wait_for_log(r"Received new config data from MQTT, will trigger event")
    booted.broker.wait_for_subscription(booted.panel.client_id, booted.manager.config_topic())
    booted.broker.wait_for_subscription(booted.panel.client_id, "nspanel/mqttmanager_%s/room/1/state" % QEMU_HOST_ADDRESS)
    # NO_SCREENSAVER blanks the display rather than showing a clock.
    booted.nextion.wait_for_page("screensaver2")
    booted.nextion.wait_for(lambda n: n.system.get("dim") == 0, what="display dimmed to 0")


@lvgl_only
def test_receives_config_and_shows_screensaver_on_lvgl(booted):
    # The same boot as above, seen through the LVGL UI: the screensaver page is "shown" (the log) on a
    # terminal session that has drawn it, and with NO_SCREENSAVER the display is dimmed to 0.
    booted.panel.wait_for_log(r"Received new config data from MQTT, will trigger event")
    booted.broker.wait_for_subscription(booted.panel.client_id, booted.manager.config_topic())
    booted.broker.wait_for_subscription(booted.panel.client_id, "nspanel/mqttmanager_%s/room/1/state" % QEMU_HOST_ADDRESS)
    booted.panel.wait_for_log(r"ScreensaverPage: Screensaver mode is 'no_screensaver'")
    assert booted.nextion.showing_terminal
    booted.nextion.wait_for(lambda n: n.system.get("dim") == 0, what="display dimmed to 0")
    assert not booted.nextion.terminal.faults


def test_announces_online_with_retained_state(booted):
    msg = booted.broker.wait_for_message("nspanel/%s/status" % booted.mac,
                                         lambda m: json.loads(m.payload).get("state") == "online")
    assert msg.retain


def test_publishes_status_report(booted):
    msg = booted.broker.wait_for_message("nspanel/%s/status_report" % booted.mac, timeout=60)
    report = pb.NSPanelStatusReport()
    report.ParseFromString(msg.payload)

    assert report.mac_address == booted.mac
    assert report.ip_address == "10.0.2.15"
    assert report.rssi == -50  # the QEMU build's stand-in for esp_wifi_sta_get_rssi()


def test_web_ui_is_served(booted):
    status, body = booted.http_get("/")
    assert status == 200 and b"<html" in body.lower()

    status, data = booted.http_get("/status_data")
    assert status == 200
    assert data.get("wifi_status")  # esp_wifi_get_mode() must not crash a panel that never initialised Wi-Fi


@nextion_only
def test_screenshot_is_409_on_nspm_tft(booted):
    # The Nextion draws NSPM's pages itself: there is nothing on the ESP32 to capture.
    with pytest.raises(urllib.error.HTTPError) as e:
        urllib.request.urlopen("http://127.0.0.1:%d/screenshot" % booted.panel.web_port, timeout=30)
    assert e.value.code == 409
    assert e.value.headers.get_content_type() == "text/plain"
    assert b"LVGL" in e.value.read()


@lvgl_only
def test_screenshot_is_served_on_lvgl(booted):
    # What the 409 above is not: the LVGL UI renders the screen itself, so there is a picture to capture.
    # (test_lvgl_terminal.py checks the pixels; this is the same endpoint's answer on the booted panel.)
    from test_lvgl_terminal import get_screenshot, png_chunks
    status, headers, body = get_screenshot(booted, "png")
    assert status == 200 and headers["Content-Type"] == "image/png"
    png_chunks(body)


def test_boots_when_its_web_port_is_taken(rig):
    # QEMU binds the ports _free_port() picked; on a busy CI runner something can take one first
    # ("Address already in use"), and QEMU exits at once. The panel must start on fresh ports instead.
    import socket

    taken = rig.panel.web_port
    with socket.socket() as squatter:
        squatter.bind(("127.0.0.1", taken))
        squatter.listen()
        rig.boot()
    assert rig.panel.web_port != taken
    status, body = rig.http_get("/")
    assert status == 200 and b"<html" in body.lower()


def test_command_sent_right_after_register_accept_is_acted_on(rig):
    # The manager can send a command (here a reboot) as soon as it has accepted the panel. The panel
    # listens for commands before it registers, so even one sent straight after the accept is handled.
    rig.manager.commands_after_accept = ["reboot"]
    rig.start()
    rig.manager.wait_for_registration(timeout=BOOT_TIMEOUT)
    # Every boot under QEMU is a POWERON_RESET (the rig restarts QEMU), so the panel's own line says why.
    rig.panel.wait_for_log(r"UpdateManager: Received command to reboot", timeout=60)
    rig.panel.wait_for_reboot(1, timeout=60)
    # And comes back: registers again after the reboot.
    rig.manager.wait_for_registration(timeout=BOOT_TIMEOUT, since=rig.manager.registration_mark())
