"""Boot, registration and the steady state a configured panel settles into."""

import json

from nspm_qemu.fake_manager import QEMU_HOST_ADDRESS
from nspm_qemu.proto import nspanel as pb


def test_registers_with_manager(rig):
    rig.start()
    request = rig.manager.wait_for_registration(timeout=180)

    assert request["mac_origin"] == rig.mac
    assert request["friendly_name"] == rig.hostname
    assert request["model"] == "sonoff"
    assert request["address"] == "10.0.2.15"  # QEMU user-mode DHCP lease, via the Ethernet path


def test_receives_config_and_shows_screensaver(booted):
    booted.panel.wait_for_log(r"Received new config data from MQTT, will trigger event")
    booted.broker.wait_for_subscription(booted.panel.client_id, booted.manager.config_topic())
    booted.broker.wait_for_subscription(booted.panel.client_id, "nspanel/mqttmanager_%s/room/1/state" % QEMU_HOST_ADDRESS)
    # NO_SCREENSAVER blanks the display rather than showing a clock.
    booted.nextion.wait_for_page("screensaver2")
    booted.nextion.wait_for(lambda n: n.system.get("dim") == 0, what="display dimmed to 0")


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
