"""The screensaver's now-playing card (the album art of the home room's first media player), on the LVGL UI.

The manager's config names the player's state topic (screensaver_media_player_topic); the panel holds
it for as long as the card is configured and decodes each NSPanelEntityState as it arrives. The card is
in the rotation only while the player plays (or buffers) and is not muted ("Screensaver card:
now_playing (4 s)"), and the album art is fetched when the state arrives, once per track and size. The
fetches are counted on the console (LvglArt: Fetched ..., at INFO; cache hit, at DEBUG) and at the
fake manager's image server (nspm_qemu/art_server.py). The pixels of the card are checked on the host
(test/host/lvgl_sim).

Needs the LVGL UI (NSPM_QEMU_TERMINAL=us|eu-on-us; us is the default), and a schema that has the
now-playing card: qemu/protobuf.ref pins a fork commit that has it; NSPM_PROTO_DIR points at another checkout.
"""

import re
import time

import pytest

from nspm_qemu.fake_nextion import TERMINAL
from nspm_qemu.proto import entity
from nspm_qemu.proto import nspanel as pb

from test_lvgl_terminal import settle, wake_to_home
from test_screensaver_cards import configure, turns, wait_for_turns

pytestmark = [
    pytest.mark.skipif(TERMINAL is None, reason="needs the LVGL UI: NSPM_QEMU_TERMINAL=us|eu-on-us (the default)"),
    pytest.mark.skipif(not hasattr(pb.NSPanelConfig, "SCREENSAVER_CARD_NOW_PLAYING"),
                       reason="the schema has no SCREENSAVER_CARD_NOW_PLAYING (see protobuf.ref, NSPM_PROTO_DIR)"),
]

Player = entity.NSPanelEntityState.MediaPlayer
PLAYER_ID = 24
CARD_SECONDS = 4

FETCHED = r"LvglArt: Fetched (\d+)x(\d+) album art"
CACHE_HIT = r"LvglArt: cache hit"
ELIGIBLE = r"ScreensaverPage: Now playing: eligible"
NOT_ELIGIBLE = r"ScreensaverPage: Now playing: not eligible"


def state_topic(rig):
    return "%s/entities/media_players/%d/state" % (rig.manager.base, PLAYER_ID)


def player_state(rig, state=Player.PLAYING, muted=False, title="Blue in Green", artist="Miles Davis", version="a", volume=30):
    update = entity.NSPanelEntityState()
    player = update.media_player
    player.media_player_id = PLAYER_ID
    player.name = "Lounge"
    player.state = state
    player.media_title = title
    player.media_artist = artist
    player.volume = volume
    player.is_muted = muted
    player.can_play = player.can_pause = player.can_set_volume = player.can_mute = True
    player.volume_step = 5
    player.album_art_url = rig.art_server.url(PLAYER_ID, version) if version else ""
    return update


def publish_player(rig, **kwargs):
    rig.broker.publish(state_topic(rig), player_state(rig, **kwargs).SerializeToString(), retain=True)


def configure_card(rig, mode=None, **kwargs):
    configure(rig, mode=mode if mode is not None else pb.NSPanelConfig.WEATHER_WITH_BACKGROUND,
              cards=((pb.NSPanelConfig.SCREENSAVER_CARD_NOW_PLAYING, CARD_SECONDS),), **kwargs)
    rig.manager.config.screensaver_media_player_topic = state_topic(rig)


def now_playing_turns(rig, since):
    return [t for t in turns(rig, since) if t[0] == "now_playing"]


def wait_until(rig, predicate, what, timeout=240):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        rig.panel.check_alive()
        time.sleep(0.5)
    raise TimeoutError("%s within %d s" % (what, timeout))


def wait_for_now_playing_turns(rig, count, since, timeout=240):
    wait_until(rig, lambda: len(now_playing_turns(rig, since)) >= count,
               "only %r of %d now-playing turns" % (turns(rig, since), count), timeout)
    return now_playing_turns(rig, since)


def fetched(rig, since=0):
    return [(int(m.group(1)), int(m.group(2))) for m in map(re.compile(FETCHED).search, rig.panel.log_lines(FETCHED, since=since)) if m]


def test_card_joins_and_leaves_the_rotation_with_playback(rig):
    configure_card(rig)
    publish_player(rig)
    rig.boot()
    rig.panel.wait_for_log(ELIGIBLE, timeout=60)
    assert wait_for_now_playing_turns(rig, 2, since=0)[0] == ("now_playing", CARD_SECONDS)

    # Paused: out at once, and it stays out.
    publish_player(rig, state=Player.PAUSED)
    rig.panel.wait_for_log(NOT_ELIGIBLE, timeout=60)
    time.sleep(2)  # a turn already under way
    mark = rig.panel.mark()
    time.sleep(2 * (3 + CARD_SECONDS) + 2)
    assert now_playing_turns(rig, mark) == [], "a paused player's card was shown"

    # A muted player is out too. Volume 0 is not muted: it is still a player playing.
    publish_player(rig, state=Player.PLAYING, muted=True)
    time.sleep(2 * (3 + CARD_SECONDS))
    assert now_playing_turns(rig, mark) == [], "a muted player's card was shown"
    publish_player(rig, state=Player.PLAYING, muted=False, volume=0)
    wait_for_now_playing_turns(rig, 1, since=mark)

    # Buffering between tracks keeps the card in.
    mark = rig.panel.mark()
    publish_player(rig, state=Player.BUFFERING, volume=0)
    wait_for_now_playing_turns(rig, 2, since=mark)
    assert not rig.nextion.terminal.faults
    rig.panel.check_alive()


def test_five_rotations_of_one_track_fetch_the_art_once(rig):
    configure_card(rig)
    publish_player(rig)
    rig.boot()
    rig.panel.wait_for_log(FETCHED, timeout=120)
    wait_for_now_playing_turns(rig, 5, since=0)

    sizes = fetched(rig)
    assert len(sizes) == 1, "fetched %r" % sizes
    size = sizes[0][0]
    # The largest square that leaves room for two lines of text: a multiple of 8, at most 256.
    assert sizes[0] == (size, size) and size % 8 == 0 and 160 <= size <= 256, sizes
    requests = rig.art_server.requested("a")
    assert [(r["w"], r["h"]) for r in requests] == [(str(size), str(size))], requests
    assert rig.panel.log_lines(CACHE_HIT) == [], "the card's hold is the cache: rotating looks nothing up"
    assert not rig.nextion.terminal.faults


def test_a_new_track_fetches_once_and_the_same_track_again_does_not(rig):
    configure_card(rig)
    publish_player(rig, version="a")
    rig.boot()
    rig.panel.wait_for_log(FETCHED, timeout=120)

    publish_player(rig, version="b", title="So What")
    wait_until(rig, lambda: len(fetched(rig)) >= 2, "no second fetch for the new track", timeout=120)
    wait_for_now_playing_turns(rig, 3, since=0)
    assert len(fetched(rig)) == 2, fetched(rig)
    assert len(rig.art_server.requested("a")) == 1 and len(rig.art_server.requested("b")) == 1

    # Pause and play the same track: its art is still in the cache.
    mark = rig.panel.mark()
    publish_player(rig, state=Player.PAUSED, version="b", title="So What")
    rig.panel.wait_for_log(NOT_ELIGIBLE, timeout=60, since=mark)
    publish_player(rig, version="b", title="So What")
    rig.panel.wait_for_log(CACHE_HIT, timeout=60, since=mark)
    wait_for_now_playing_turns(rig, 1, since=mark)
    assert len(fetched(rig)) == 2, fetched(rig)
    assert len(rig.art_server.requested("b")) == 1


def test_art_is_fetched_while_another_card_is_showing(rig):
    """A track change while the weather shows is fetched when the state arrives, not when the card's turn comes."""
    configure_card(rig)
    publish_player(rig, version="a")
    rig.boot()
    rig.panel.wait_for_log(FETCHED, timeout=120)
    mark = rig.panel.mark()
    publish_player(rig, version="c", title="All Blues")
    rig.panel.wait_for_log(FETCHED, timeout=60, since=mark)
    assert len(rig.art_server.requested("c")) == 1


def test_a_player_with_no_art_is_still_a_card(rig):
    configure_card(rig)
    publish_player(rig, version="")
    rig.boot()
    wait_for_now_playing_turns(rig, 1, since=0)
    assert fetched(rig) == []
    assert rig.art_server.requested() == []


def test_datetime_mode_with_only_the_card_shows_the_clock_until_something_plays(rig):
    configure_card(rig, mode=pb.NSPanelConfig.DATETIME_WITH_BACKGROUND)
    publish_player(rig, state=Player.PAUSED)
    rig.boot()
    time.sleep(8)
    assert turns(rig, 0) == [], "nothing plays: the centred clock, no rotation"

    mark = rig.panel.mark()
    publish_player(rig)
    wait_for_now_playing_turns(rig, 1, since=mark)
    rig.panel.wait_for_log(FETCHED, timeout=120)

    mark = rig.panel.mark()
    publish_player(rig, state=Player.PAUSED)
    rig.panel.wait_for_log(NOT_ELIGIBLE, timeout=60, since=mark)
    settle(rig.nextion)
    assert not rig.nextion.terminal.faults
    rig.panel.check_alive()


def test_no_screensaver_follows_the_player_without_showing_it(rig):
    configure_card(rig, mode=pb.NSPanelConfig.NO_SCREENSAVER)
    publish_player(rig)
    rig.boot()
    rig.panel.wait_for_log(ELIGIBLE, timeout=60)
    time.sleep(10)
    assert turns(rig, 0) == []
    rig.panel.check_alive()


@pytest.fixture
def entities_rig(rig):
    """The home room's entities page has the player in its first slot."""
    if TERMINAL != "us":
        pytest.skip("touch coordinates are for the US layout")
    configure_card(rig)
    page = rig.manager.entity_pages[11]
    del page.entities[:]
    slot = pb.NSPanelRoomEntitiesPage.EntitySlot
    page.entities.add(room_view_position=0, name="Lounge", icon="s", pco=65535, pco2=65535,
                      type=slot.ENTITY_TYPE_MEDIA_PLAYER, id=PLAYER_ID, mqtt_state_topic=state_topic(rig))
    return rig


# Where things are on the US screens, as they are shown (tests/.../screens/*.png).
US_ENTITIES_BUTTON = (300, 22)
US_FIRST_SLOT = (160, 110)


def test_media_page_for_the_same_player_opens_from_the_cache_and_the_card_follows_on(entities_rig):
    rig = entities_rig
    topic = state_topic(rig)
    publish_player(rig)
    rig.boot()
    rig.panel.wait_for_log(FETCHED, timeout=120)
    (card_size, _), = fetched(rig)

    wake_to_home(rig)
    mark = rig.panel.mark()
    events = rig.broker.event_mark()
    rig.nextion.tap_screen(*US_ENTITIES_BUTTON)
    rig.broker.wait_for_subscription(rig.panel.client_id, rig.manager.base + "/entity_pages/11/state", since=events)
    settle(rig.nextion)
    rig.nextion.tap_screen(*US_FIRST_SLOT)

    # The media page draws the cached state at once (its topic is the card's), and asks for art at its own size.
    rig.panel.wait_for_log(r"MediaPlayerPage: Media player %d 'Lounge': Playing" % PLAYER_ID, timeout=60, since=mark)
    rig.panel.wait_for_log(r"LvglArt: Fetched 180x180 album art", timeout=120, since=mark)
    assert not [e for e in rig.broker.events[events:] if e[0] == "unsubscribe" and e[2] == topic], "the card's hold went"
    assert topic in rig.broker.subscriptions(rig.panel.client_id)

    # The screensaver comes back after the idle timeout, and the card follows the player again.
    mark = rig.panel.mark()
    rig.panel.wait_for_log(r"Showing screensaver page", timeout=240, since=mark)
    publish_player(rig, version="d", title="Flamenco Sketches")
    rig.panel.wait_for_log(FETCHED, timeout=120, since=mark)
    wait_for_now_playing_turns(rig, 2, since=mark)
    assert fetched(rig, since=mark)[0] == (card_size, card_size)
    assert len(rig.art_server.requested("a")) == 1, "the first track's art was fetched again"
    assert not rig.nextion.terminal.faults
