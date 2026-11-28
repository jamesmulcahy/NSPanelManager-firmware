# Brief: play NSPanel voice recordings through the house broadcast (Home Assistant side)

For a Claude session with access to the user's Home Assistant (via its HA MCP). The panel firmware is
built. This brief covers the HA end only. The firmware side's full design is `docs/VOICE_RECORDER.md`
in the same repo (branch `feat/p4-voice`), but everything you need is here.

## Goal

A wall panel (a Waveshare ESP32-P4 running the user's NSPanelManager firmware) has a hold-to-talk
button. When the user releases it, the panel keeps the recording, serves it as a WAV at a short-lived
URL, and announces that URL over MQTT. **Make HA play that recording on the house's speakers through
the existing broadcast mechanism**: the script `script.broadcast_voice_prompted`, which already
accepts either an audio URL or text.

There is no speech-to-text and no TTS. The user's own voice is what gets played. The user finds HA
Assist's recognition poor, so don't route the audio through Assist.

## The interface: what the panel guarantees (conform to this)

### MQTT message
| | |
|---|---|
| Topic | `nspanel/<mac>/voice/recording`. `<mac>` is the panel's MAC, uppercase with colons, e.g. `nspanel/C0:49:EF:60:AD:68/voice/recording`. Subscribe with `nspanel/+/voice/recording` |
| QoS | 1 |
| Retained | **No.** A recording made while HA is down is lost, by design, so an old message never replays |
| When | Once per recording, right after the user releases the button (or at the 15 s limit) |

Payload (JSON). Every field is always present, except where noted:
```json
{
  "url": "http://192.168.1.123/rec/3f9c0a1e7b2d4c6e8f00112233445566.wav",
  "token": "3f9c0a1e7b2d4c6e8f00112233445566",
  "duration_ms": 4200,
  "bytes": 134444,
  "format": "wav",
  "sample_rate": 16000,
  "expires_in": 58,
  "sequence": 7,
  "nspanel_id": 14,
  "name": "Office P4",
  "mac": "AA:BB:CC:DD:EE:FF"
}
```
| Field | Meaning |
|---|---|
| `url` | Where to fetch the audio. Plain HTTP, port 80, the panel's own LAN IP. **Use this; don't build the URL yourself.** |
| `token` | The 32-hex part of the URL |
| `duration_ms` | Recording length, 300 to 15000 ms |
| `bytes` | Exact file size (44-byte WAV header + PCM) |
| `format`, `sample_rate` | Always `wav` and `16000` for now |
| `expires_in` | Seconds left before the URL stops working (at most 60). Relative, because the panel has no guaranteed wall clock |
| `sequence` | Increases with each recording since the panel booted |
| `nspanel_id` | The panel's id in NSPanelManager (-1 if not registered yet) |
| `name` | The panel's name |
| `mac` | Same as in the topic |
| `room` | **Not sent yet.** It may be added later. Treat it as optional (`| default('')`) |

### The audio URL
- **Format:** WAV (RIFF), PCM 16-bit signed little-endian, **mono, 16 kHz**, at most 15 s (about 480 KB).
  It is already normalised: speech peaks around -8 dBFS and never above -3 dBFS.
- **Fetching:** the URL can be fetched **any number of times** (HA and each speaker may all fetch it), with
  `GET` or `HEAD`. A single `Range: bytes=a-b` request gets a 206. Responses carry `Content-Type: audio/wav`,
  `Content-Length`, `Accept-Ranges: bytes` and `Cache-Control: no-store`.
- **Lifetime: 60 s after the recording ends.** After that it returns `410 Gone`, and `404` once a newer
  recording has reused its slot. The panel keeps the last two recordings. **Playback has to start, and
  every speaker has to have fetched the file, within about a minute.** If the broadcast mechanism waits for
  something (a queue, other announcements, speakers waking up) longer than that, HA must fetch a copy first
  (see below).
- **Reachability:** the speakers, or HA if it proxies, must be able to reach the panel's IP on port 80. HA is
  at 192.168.1.42; panels are on 192.168.1.x and some on 192.168.0.x.

### Testing without pressing the button
Anything that can publish MQTT can make the panel record:
```
topic   nspanel/<mac>/command
payload {"command":"voice_record","action":"start","seconds":5}
```
That records 5 s (1-15) from the panel's mics and then publishes the message above. In HA, use
`mqtt.publish` with those values. `"action":"stop"` and `"action":"cancel"` also exist. To find the P4
panel's MAC, watch `nspanel/+/voice/recording` while the user presses the button once, or ask the user.

## What to do

1. **Read `script.broadcast_voice_prompted`.** Work out:
   - its fields (the audio-URL one, and any for source, target or exclusions);
   - how it plays audio (`media_player.play_media` with `announce`? a TTS or other integration?);
   - which speakers it targets by default;
   - whether it queues or waits.

   Don't change its behaviour for its existing callers. If it needs a change, ask the user first and keep
   it backward compatible.
2. **Check that the speakers can play the file as served:** 16 kHz mono WAV over plain HTTP from an IP
   on the LAN.
   - Sonos and Cast generally handle WAV. Alexa/Echo devices don't play arbitrary URLs.
   - If some can't, or the timing in the next step doesn't fit, use a fallback. HA downloads the URL as
     soon as the message arrives (e.g. into `/config/www/nspanel_voice/<token>.wav`, served as
     `/local/...`, or into the media source), converting it if a speaker needs a different format. It then
     broadcasts that copy and deletes old copies.
   - Report which path you took and why.
3. **Targets:** each panel's recording goes to a fixed set of speakers. There's no picker on the panel. Later,
   the panel's config in NSPanelManager will carry the targets (phase 4). For now:
   - Use the script's normal broadcast targets.
   - Ask the user whether a panel's own room should be left out (the payload has no `room` yet; map `mac`
     or `name` to a room in the automation if needed).
4. **Create the automation**, roughly like this. Adjust the field names to the script's real ones:
   ```yaml
   alias: "NSPanel voice broadcast"
   mode: queued
   max: 5
   triggers:
     - trigger: mqtt
       topic: "nspanel/+/voice/recording"
   conditions:
     - condition: template
       value_template: "{{ trigger.payload_json.url is defined }}"
   actions:
     - action: script.broadcast_voice_prompted
       data:
         audio_url: "{{ trigger.payload_json.url }}"   # the script's real field name
         # source / exclusions as the script supports, from trigger.payload_json.name / .mac
   ```
   Because of the 60 s lifetime, keep `max` small. Either make sure a queued run still starts well inside a
   minute, or take the fetch-a-copy fallback.
5. **Test it:**
   - **Without the panel:** `mqtt.publish` a fake message on `nspanel/test/voice/recording` with `url`
     pointing at any reachable short WAV, and confirm it plays on the speakers.
   - **With the panel:** send the `voice_record` command above, confirm the recording plays, and check the
     timing (message arrival to playback start).
   - **Expiry:** fetch a URL more than 60 s old and confirm it returns 410, so you know what a late fetch looks
     like in the logs.
6. **Report back to the user:**
   - the automation YAML exactly as created;
   - the script's real field names;
   - which playback path you used (direct or a copy, and any conversion);
   - the measured delay from release to playback;
   - anything in the interface above that doesn't suit the house, such as a format the speakers reject, a
     lifetime that's too short, or a field you need but don't get. Report it rather than working around it
     silently: the firmware side can change, and the user will take it back to that session.

## Out of scope
- Changing the panel firmware or this interface.
- Recording audio on HA's side or speech recognition.
- Per-panel target configuration in NSPanelManager (phase 4, firmware/manager side).
