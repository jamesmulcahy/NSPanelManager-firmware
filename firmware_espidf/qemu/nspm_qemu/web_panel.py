"""Entry point (qemu/web_panel.sh): boots one emulated panel and serves its screen in a browser,
to use by hand. QEMU runs the firmware against the fake manager and broker, and nextion-tools'
viewer shows the display: click and drag on it to touch the panel.

    qemu/web_panel.sh [--http 8080] [--bind 127.0.0.1] [--rotation 0|90|180|270] [--weather]

It prints the viewer's URL once the panel is up, then each command the panel sends the manager
as it arrives. Ctrl-C stops everything. The console log, broker log and screenshots are kept in
qemu/build/web_panel/ (--workdir for another place).
"""

import argparse
import os
import sys
import time

from nextion_tools.emu.viewer import Viewer

from .proto import nspanel as pb
from .rig import Rig

QEMU_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="qemu/web_panel.sh", description="Boot an emulated panel and use it from a browser.")
    parser.add_argument("--http", type=int, default=8080, metavar="PORT", help="the viewer's port (default 8080)")
    parser.add_argument("--bind", default="127.0.0.1",
                        help="the viewer's address (default 127.0.0.1; anyone who can reach it can touch the panel)")
    parser.add_argument("--rotation", type=int, default=0, choices=(0, 90, 180, 270),
                        help="turn the screen this many degrees clockwise")
    parser.add_argument("--weather", action="store_true", help="the weather screensaver, with a forecast")
    parser.add_argument("--workdir", default=os.path.join(QEMU_DIR, "build", "web_panel"))
    args = parser.parse_args(argv)

    rig = Rig(args.workdir)
    rig.nextion.rotation = args.rotation
    if args.weather:
        rig.manager.config.screensaver_mode = pb.NSPanelConfig.WEATHER_WITH_BACKGROUND
    viewer = Viewer(rig.nextion, args.bind, args.http)
    try:
        print("booting the panel (about a minute; console log in %s)..." % args.workdir, flush=True)
        rig.boot()
        if args.weather:
            rig.manager.publish_weather()
        print("panel up: open %s" % viewer.url, flush=True)
        seen = rig.manager.command_mark()
        while True:
            rig.panel.check_alive()
            commands = rig.manager.commands
            for cmd in commands[seen:]:
                print("manager <- %s" % cmd.WhichOneof("CommandData"), flush=True)
            seen = len(commands)
            time.sleep(0.2)
    except KeyboardInterrupt:
        return 0
    except Exception as e:
        print("stopped: %s" % e, file=sys.stderr)
        print(rig.panel.tail(40), file=sys.stderr)
        return 1
    finally:
        viewer.close()
        rig.stop()


if __name__ == "__main__":
    sys.exit(main())
