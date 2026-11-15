"""The panel used from a browser, as qemu/web_panel.sh serves it: only through the viewer's HTTP."""

import io
import json
import urllib.request

import pytest
from PIL import Image

from nextion_tools.emu.viewer import Viewer
from nspm_qemu import gui
from nspm_qemu.proto import nspanel as pb


@pytest.fixture
def viewer(booted):
    v = Viewer(booted.nextion)
    yield v
    v.close()


def get(viewer, path):
    with urllib.request.urlopen(viewer.url.rstrip("/") + path, timeout=10) as r:
        return r.read()


def tap(viewer, x, y):
    for kind in ("down", "up"):
        req = urllib.request.Request(viewer.url + "touch", data=json.dumps({"kind": kind, "x": x, "y": y}).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=10) as r:
            assert json.load(r) == {"ok": True}


def test_wake_the_panel_and_turn_on_the_ceiling_lights_from_the_browser(booted, viewer):
    n = booted.nextion
    n.wait_for(lambda d: d.page in ("screensaver", "screensaver2"), what="screensaver shown")
    w, h = n.screen.page(n.page).size
    tap(viewer, w // 2, h // 2)

    n.wait_for_page("home")
    n.wait_for(lambda d: d.get("home.room.txt") is not None, what="home page rendered")
    state = json.loads(get(viewer, "/state"))
    assert state["page"] == "home" and state["connected"]
    with Image.open(io.BytesIO(get(viewer, "/frame.png"))) as im:
        assert im.size == n.screen.page("home").size

    mark = booted.manager.command_mark()
    x, y = n.point_on(gui.HOME_CEILING)
    tap(viewer, x, y)
    cmd = booted.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)
    assert cmd.first_page_turn_on.affect_lights == pb.NSPanelMQTTManagerCommand.CEILING_LIGHTS
