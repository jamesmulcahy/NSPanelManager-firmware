"""Recovery after the broker connection drops, as on a flaky network."""

import re
import time

from nspm_qemu import gui

# NSPM_ConfigManager::_task_send_register_request re-sends until accepted, this often.
REGISTER_REQUEST_PERIOD = 5


def test_resubscribes_and_reregisters_after_connection_drop(home):
    client = home.panel.client_id
    before = set(home.broker.subscriptions(client))
    assert home.manager.config_topic() in before

    reg_mark = home.manager.registration_mark()
    events = home.broker.event_mark()
    page_mark = len(home.nextion.page_history)
    assert home.broker.drop_client(client) == 1

    home.broker.wait_for_event("connect", client, since=events, timeout=120)
    home.manager.wait_for_registration(since=reg_mark, timeout=60)
    home.panel.wait_until(lambda: before <= set(home.broker.subscriptions(client)), timeout=60,
                          what="all subscriptions restored (missing: %s)" % (before - set(home.broker.subscriptions(client))))

    # Losing the broker means losing the manager, so the panel shows "Lost
    # connection to manager." and, once the manager is back, its screensaver.
    home.nextion.wait_for(lambda n: "screensaver2" in n.page_history[page_mark:], timeout=60,
                          what="screensaver shown again")

    # Still usable afterwards.
    home.wake_to_home()
    mark = home.manager.command_mark()
    home.nextion.press(gui.HOME_CEILING)
    home.manager.wait_for_command(lambda c: c.WhichOneof("CommandData") == "first_page_turn_on", since=mark)


def test_last_will_marks_panel_offline(home):
    mark = home.broker.mark()

    home.broker.drop_client(home.panel.client_id)

    will = home.broker.wait_for_message("nspanel/%s/status" % home.mac, lambda m: b"offline" in m.payload,
                                        since=mark, timeout=10)
    assert will.retain  # MqttManager sets last_will.retain


def test_reregisters_after_rename_and_reconnect(home):
    """A panel renamed in the manager must still complete registration after a reconnect.

    The manager sends register_accept on nspanel/<friendly_name>/command and on
    nspanel/<mac>/command. The panel adopts the manager's name as its hostname,
    and its register_request carries the new name, but it still listens on the old
    name's topic. It must take the accept from the MAC topic, which a rename can't
    change.
    """
    client = home.panel.client_id
    home.manager.config.name = "Renamed Panel"
    home.manager.publish_config()
    home.panel.wait_for_log(r"Updating internal WiFi hostname to match manager provided friendly name")

    reg_mark = home.manager.registration_mark()
    log_mark = home.panel.mark()
    home.broker.drop_client(client)

    request = home.manager.wait_for_registration(since=reg_mark, timeout=120)
    assert request["friendly_name"] == "Renamed Panel"
    home.panel.wait_for_log(r"Received register_accept from manager", since=log_mark, timeout=30)

    # Accepted, so the panel stops asking.
    settled = home.manager.registration_mark()
    home.panel.wait_for_log(r"Subscribed to '%s'" % re.escape(home.manager.config_topic()), since=log_mark, timeout=30)
    time.sleep(2 * REGISTER_REQUEST_PERIOD + 1)  # long enough for two more, if it were still sending
    assert home.manager.registration_mark() - settled <= 1, "panel kept sending register_request after being accepted"
