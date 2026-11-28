# Voice recorder (custom_pcb) — phase 2a design

Hold-to-talk on the panel → a WAV in PSRAM → `http://<panel IP>/rec/<token>.wav` → an MQTT message → a
Home Assistant automation calls `script.broadcast_voice_prompted` with the URL, and the speakers fetch and
play it. No STT, no TTS, no models (GSC + WebRTC NS run without the `model` partition).

Interface: [`lib/VoiceRecorder/VoiceRecorder.hpp`](../lib/VoiceRecorder/VoiceRecorder.hpp); implemented in
`VoiceRecorder.cpp` (phase 2b), with the pure parts in `VoiceRecorderPure.hpp` (host tests:
`test/host/voice_recorder/build.sh`). This file is the reasoning and the parts that are not C++.

## 1. The pieces and their tasks

| Piece | Task (priority) | Does |
|---|---|---|
| `VoiceCapture` | `voice_capture` (6) | I2S/ES7210, GSC + NS (phase 1b), calls the recorder's **sink** with mono 16 kHz chunks |
| `VoiceRecorder` API | caller (LVGL task; `mqtt_dispatch` for the test command) | `start` / `stop` / `stop_and_send` / `cancel`, serialised by `_api_mutex` |
| `VoiceRecorder` worker | `voice_rec` (3, 4 KB, internal RAM) | MQTT publish of announcements, the auto-stop at `MAX_FRAMES` and `stop_after_ms` |
| `VoiceRecorder::serve` | `httpd` (5) | `GET`/`HEAD /rec/<token>.wav` |

The worker exists because `MqttManager::publish()` can block for seconds on a weak link (see
`MqttManager.hpp`) and must not run on the LVGL task, and because the auto-stop is triggered from the
capture task's sink, which must never block.

## 2. Streaming API needed from VoiceCapture (for phase 1b to adapt to)

`record()` stays as it is (the temporary `/rec/test.wav` uses it). Added beside it, sharing `_busy`
(so `record()` and a stream exclude each other with `ESP_ERR_INVALID_STATE`):

```cpp
class VoiceCapture {
public:
  /**
   * Capture task: `frames` frames of processed audio, 16 kHz mono 16-bit (GSC + NS output, or MIC1 when
   * processing is off). Every chunk is CHUNK_FRAMES (320, 20 ms) except possibly the last. Must not block
   * or take a lock that a blocking task holds; budget < 1 ms. `mono` is only valid during the call.
   */
  using Sink = void (*)(const int16_t *mono, size_t frames, void *context);

  /**
   * Start streaming to `sink`. Blocks until the capture task has opened the codec (or failed), then
   * returns; the SETTLE_FRAMES (100 ms) are dropped before the first sink call. Errors:
   * ESP_ERR_INVALID_ARG (no sink), ESP_ERR_INVALID_STATE (record() or another stream running),
   * ESP_ERR_NO_MEM, or the codec's failure.
   */
  static esp_err_t start(Sink sink, void *context, int gain_db = DEFAULT_GAIN_DB);

  /**
   * Stop streaming. Blocks until the capture task has returned from its last sink call and closed the
   * codec (at most one chunk plus one I2S read timeout, ~1 s worst case; ~20 ms normally). After it
   * returns the sink is never called again for this stream. Returns ESP_OK, ESP_ERR_INVALID_STATE (no
   * stream), or ESP_FAIL if an I2S read failed during the stream (the stream ended itself early; stop()
   * must still be called to release it).
   */
  static esp_err_t stop();
};
```

Implementation hint for 1b: the capture task's loop becomes "read chunk → `_process()` into an internal
mono scratch buffer → either copy into `request->out` (record) or call the sink (stream)", with the
end condition a frame count (record) or a `stop` flag checked once per chunk (stream). `_process()`'s
output must therefore be **mono** for the stream path; `record()` may keep its 3-channel raw mode.

## 3. Buffers, ownership and locking

- **Allocation:** on the first `start()`, `SLOTS = 2` buffers of `SLOT_BYTES` (480 044 B) each with
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`: **0.96 MB of the board's 32 MB PSRAM**, never freed. Never
  freeing is what makes use-after-free impossible; the rules below only prevent a buffer being
  *overwritten* while it is being sent.
- **Max length 15 s** (the user's choice; 480 KB per slot): an announcement, not a dictation; two slots
  stay under 1 MB and a full download well under a second on Wi-Fi. Changing it is one constant
  (`MAX_SECONDS`), ~32 KB per second per slot. At the limit the recording **sends itself** (the worker
  runs `stop()` + `announce()`, as if the user let go).
- **Slot:**

  | Field | Written by | Under |
  |---|---|---|
  | `state` (FREE, RECORDING, READY) | API (start/stop/cancel) | `_slots_mutex` |
  | `token`, `expires_us`, `sequence` | `stop()` | `_slots_mutex` |
  | `readers` (count of `serve()` calls sending from it) | `serve()` | `_slots_mutex` |
  | `frames`, sample data | the sink (capture task) | none: only while RECORDING, which `serve()` never reads |
  | WAV header (first 44 B) | `stop()`, after `VoiceCapture::stop()` returned | none: slot not yet READY |

- **Choosing a slot in `start()`** (under `_slots_mutex`): a slot with `readers == 0`, preferring FREE,
  then READY-and-expired, then the oldest READY. Its token is invalidated at once (`state = RECORDING`).
  If both slots have readers → `ESP_ERR_NOT_FINISHED` (practically only when two speakers are fetching
  both previous recordings at the same moment). So: **starting a new recording never disturbs the
  previous one**, which stays downloadable for its TTL; the one before that is overwritten.
- **`serve()`:** lock, find the READY slot with this token and `now < expires_us`, `readers++`, copy the
  pointer and length, unlock; send **without the lock**; lock, `readers--`, unlock. `start()` can't pick
  a slot with readers, so the bytes can't change under a send.
- **The sink** writes to `slot->data + frames` only while `frames < MAX_FRAMES`; at the limit it sets
  `State::FULL` and, with `auto_send_at_limit`, posts "auto-stop" to the worker (`xQueueSend`, 0 timeout).
  It also updates `_level` and `_frames` (atomics).
- **`stop()` / `cancel()`** call `VoiceCapture::stop()` first, so the sink is finished with the slot
  before the header is written or the slot freed. Everything after that runs under `_slots_mutex`.
- `_api_mutex` serialises start/stop/cancel (UI vs. the test command vs. the worker's auto-stop); the
  worker's auto-stop does nothing if the state is no longer FULL / the recording it was set for.
- Locks are never held across `VoiceCapture::start/stop`, a socket send or an MQTT publish. Order:
  `_api_mutex` then `_slots_mutex`.

## 4. Token

| Property | Decision |
|---|---|
| Format | 16 bytes from `esp_fill_random()` (hardware RNG; true random with Wi-Fi up), 32 lowercase hex chars |
| URL | `http://<WiFiManager::ip_string()>/rec/<token>.wav` (port 80, `WebManager::HTTP_PORT`) |
| Lifetime | `TOKEN_TTL_S = 60` s from `stop()`, or until its slot is reused, whichever is first. Checked when a request arrives: a download under way at expiry finishes |
| Use | **Multi-use** within its lifetime: HA, Sonos and Cast fetch the URL themselves, often more than once (a HEAD, then ranged GETs, and one fetch per speaker) |
| Unknown token / bad form | `404 Not Found` |
| Known but expired | `410 Gone`: the slot still holds the token, `now >= expires_us` (more than 60 s after `stop()`). Once the slot is reused for a newer recording the token is gone: 404 |
| Comparison | constant-time over the 32 chars |

Not single-use because it would break multi-speaker groups and players that probe first; the 128-bit
token on the LAN plus the 60 s expiry is the protection. 60 s is ample for HA to react to the MQTT
message, but it means a broadcast that waits behind others (the automation is `mode: queued`) for over
a minute gets a 410; `max: 5` and 15 s recordings keep that unlikely.

## 5. HTTP: `GET|HEAD /rec/<token>.wav`

- WebManager sets `_config.uri_match_fn = httpd_uri_match_wildcard` and registers `"/rec/*"` for
  `HTTP_GET` and `HTTP_HEAD`, both to `VoiceRecorder::serve`, **after** `/rec/test.wav` (first match
  wins). The exact URIs already registered are unaffected by the wildcard matcher. 16 handlers suffice
  (13 + 2 = 15 on custom_pcb; 17 with `NSPM_HEAP_DIAG`, so raise `max_uri_handlers` to 18).
- Path parsing: `req->uri` minus `/rec/`, must be exactly 32 hex chars + `.wav` (query string ignored).
- Headers on 200/206: `Content-Type: audio/wav`, `Accept-Ranges: bytes`, `Cache-Control: no-store`,
  `Content-Length` (set by `httpd_resp_send`), on 206 also `Content-Range: bytes a-b/total`.
- **Range:** one range only: `bytes=a-b`, `bytes=a-`, `bytes=-n`. Anything else (multiple ranges,
  bad syntax) → ignore it and send 200 (RFC 9110 allows that). `a >= total` → `416` with
  `Content-Range: bytes */total`.
- **Sending:** `httpd_resp_send(req, data + a, len)` straight from PSRAM, not `httpd_resp_send_chunk`
  (which forces chunked transfer encoding, so no Content-Length — some players need it). It writes in
  socket-sized pieces itself; the slot's `readers` count keeps the bytes stable for the whole call.
  HEAD: `httpd_resp_send` always writes `Content-Length: <buf_len>` itself (so 0 for an empty body, and
  a second header can't be added), so HEAD writes its status line and headers by hand with the raw
  `httpd_send()`, with the real length and no body.
- The web server has one task, so a 480 KB send holds other requests for ~0.5 s. Accepted.

## 6. MQTT

Topic **`nspanel/<mac>/voice/recording`** (beside `nspanel/<mac>/screen_state` etc.; `<mac>` as
`WiFiManager::mac_string()`). **Not retained** (a retained message would make HA replay an old
broadcast on restart). **QoS 1**: phase 2b adds an `int qos = 0` parameter to `MqttManager::publish()`
(today every publish is QoS 0); the voice message passes 1.

```json
{
  "url": "http://192.168.1.165/rec/3f9c0a1e7b2d4c6e8f00112233445566.wav",
  "token": "3f9c0a1e7b2d4c6e8f00112233445566",
  "duration_ms": 4200,
  "bytes": 134444,
  "format": "wav",
  "sample_rate": 16000,
  "expires_in": 60,
  "sequence": 7,
  "nspanel_id": 1,
  "name": "Kitchen Panel",
  "mac": "C0:49:EF:60:AD:68",
  "room": "Kitchen"
}
```

`nspanel_id` from `NSPM_ConfigManager::get_nspanel_id()` (-1 before registration), `name` from
`ConfigManager::wifi_hostname` or the manager's name, `room` the panel's home room name if known, else
omitted. `expires_in` is relative (no wall clock guaranteed). Built with nlohmann::json like the other
messages.

**Test trigger** (until the UI, phase 3): on the existing `nspanel/<mac>/command`, handled in
`UpdateManager::_mqtt_message_handler` next to `lvgl_tap`, which calls `VoiceRecorder::handle_command()`:

| Payload | Effect |
|---|---|
| `{"command":"voice_record","action":"start","seconds":5}` | record 5 s (1–15), then stop and announce |
| `{"command":"voice_record","action":"start"}` | record until `stop`/`cancel` (or the 15 s limit, which sends) |
| `{"command":"voice_record","action":"stop"}` | `stop_and_send()` |
| `{"command":"voice_record","action":"cancel"}` | `cancel()` |

```sh
mosquitto_pub -t 'nspanel/C0:49:EF:60:AD:68/command' -m '{"command":"voice_record","action":"start","seconds":5}'
mosquitto_sub -v -t 'nspanel/+/voice/recording'
```

## 7. Home Assistant

```yaml
# configuration.yaml / automations.yaml. Final form; only the script's field names remain to be confirmed.
automation:
  - alias: "NSPanel voice broadcast"
    mode: queued
    max: 5
    trigger:
      - platform: mqtt
        topic: "nspanel/+/voice/recording"
    condition:
      - condition: template
        value_template: "{{ trigger.payload_json.url is defined }}"
    action:
      - service: script.broadcast_voice_prompted
        data:
          # TO BE CONFIRMED BY THE USER: the field names below (audio_url, source) are guesses; use the ones
          # script.broadcast_voice_prompted actually declares. The URL is the only value it must get.
          audio_url: "{{ trigger.payload_json.url }}"
          source: "{{ trigger.payload_json.name }}"
          # e.g. exclude the speaker in the panel's room (when the payload has "room"):
          # exclude_room: "{{ trigger.payload_json.room | default('') }}"
```

The audio is fetched by HA or by the speakers themselves from the panel's IP, so they must reach it
on port 80 (same LAN/VLAN or a firewall rule). Phase 4 may route this through MQTTManager instead
(e.g. the manager re-hosting the file), which the payload already has what it needs for.

## 8. Telemetry hooks (phase 5; counters in `PanelMetrics`, named now)

| Counter | Counts |
|---|---|
| `nspanel_voice_recordings_total{outcome="sent"}` | stop() OK and announce published |
| `nspanel_voice_recordings_total{outcome="cancelled"}` | cancel() |
| `nspanel_voice_recordings_total{outcome="too_short"}` | stop() under `MIN_MS` |
| `nspanel_voice_recordings_total{outcome="capture_failed"}` | start() or the stream failed in VoiceCapture |
| `nspanel_voice_recordings_total{outcome="busy"}` | start() refused (`INVALID_STATE` / `NOT_FINISHED`) |
| `nspanel_voice_recordings_limit_total` | recordings that hit `MAX_FRAMES` |
| `nspanel_voice_recorded_seconds_total` | seconds of audio kept (sum of `duration_ms` / 1000) |
| `nspanel_voice_announce_failures_total` | MQTT publish failed or was not connected |
| `nspanel_voice_http_requests_total{status="200"\|"206"\|"404"\|"410"\|"416"}` | serve() responses |
| `nspanel_voice_http_bytes_total` | bytes sent by serve() |
| `nspanel_voice_http_aborted_total` | downloads that did not finish (client gone, stalled, or over the 30 s budget) |
| `nspanel_voice_sink_overruns_total` | sink calls dropped because the slot was full or no slot was RECORDING |

## 9. Phase 3a UI decisions (for 3b)

| Decision | Detail |
|---|---|
| Where | A hard-coded mic button on the home page (`HomeView`), beside the ceiling/table tiles and the sliders. Not configurable, no layout refactor |
| Which boards | Shown only on boards with microphones (`BOARD_CUSTOM` today); absent elsewhere |
| Doesn't fit | If the layout has no room for it, the panel logs a warning (`ESP_LOGW`) and shows no button |
| Gesture | Press = `start()`, release = `stop_and_send()`. **No slide-to-cancel.** `cancel()` stays for the test command and for a hold the UI loses (page left, screen off) |
| While holding | A level meter only, from `level()` polled by an LVGL timer (~50 ms) |
| After release | A toast: "Failed" at once if `stop_and_send()` returns an error (including too short, under `MIN_MS = 300` ms); otherwise "Sent" or "Failed" when the result listener (`set_result_listener()`, called on `voice_rec` after the publish) reports, handed to LVGL with `lv_async_call` |
| At 15 s | Sends automatically (`auto_send_at_limit`); the UI shows the same toast |
| Targets | Fixed per panel: which speakers play it is decided on the HA side now and by the phase 4 panel config later; no picker on the panel |

## 10. Settled with the user

1. Max length **15 s**.
2. At the limit it **sends automatically**.
3. The URL may be fetched **any number of times** and **expires 60 s** after `stop()`.
4. The MQTT message is **not retained** (a recording made while HA restarts is lost).

## 11. As built (phase 2b)

- `VoiceCapture::start(sink, context, const Options &)` takes Options, not just a gain; the one place the
  recorder chooses them is `VoiceRecorder::capture_options()`. The `GAIN_DB` constant was dropped (it was
  30 while the options say 36).
- The `voice_rec` task has a 6 KB stack (not 4 KB), unmeasured on hardware; the JSON, the log line and the
  MQTT publish run on it.
- `room` is not in the payload: the room's name is not available without RoomManager's state.
- `expires_in` is the time left when the message is published (at most 60), not a constant 60.
- `stop_after_ms` is timed by the worker's 100 ms poll, so a test recording ends up to 100 ms late.
- Test: `mosquitto_pub -t 'nspanel/<mac>/command' -m '{"command":"voice_record","action":"start","seconds":5}'`,
  `mosquitto_sub -v -t 'nspanel/+/voice/recording'`, then `curl -sv -o /tmp/r.wav <url>`, `curl -sI <url>`,
  `curl -s -H 'Range: bytes=0-99' -o /dev/null -w '%{http_code}\n' <url>` (206), and the same URL after 60 s (410).

### 11b. As built (phase 5, hardening)

**Download speed (TCP windows).** `sdkconfig.custom_pcb` has `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` and
`CONFIG_LWIP_TCP_WND_DEFAULT` at 31680 (22 x MSS 1440; they were 5760, 4 x MSS) and `CONFIG_LWIP_TCP_RECVMBOX_SIZE`
at 24 (the Kconfig's recommendation, WND/MSS + 2; it was 6). They are P4-only overrides:
`tools/sync_sdkconfig_custom_pcb.py` keeps them (`KEEP_P4`), and `sdkconfig.original_sonoff` is unchanged.
Constraints checked: SND_BUF is at least 2 x MSS; `TCP_SND_QUEUELEN` is derived (4 x SND_BUF / MSS = 88 pbufs);
without window scaling both are capped at 65535; ESP-Hosted's SDIO TX queue (20 entries) blocks when full, it does
not drop, so it needed no change. RAM, all internal heap (lwIP's pbufs come from the heap, and allocations under
16 KB are internal): what is queued for send is only what the peer has not yet ACKed, at most SND_BUF = 31 KB plus
pbuf/header overhead (about 34 KB) per connection that is sending; the receive side holds data only until the web
server reads it (at most WND = 31 KB, normally a few KB); a mailbox slot is 4 bytes. A recording's download is the
only large send, and the web server has one task, so the normal cost is about 34 KB for the 2-4 s a recording is being
fetched and nothing otherwise. Worst case, `max_open_sockets` = 4 connections each with a full send queue (an
upper bound: one serving task sends to one at a time): 4 x 34 KB = about 136 KB of the P4's internal RAM.

**Dead downloads.** `serve()` holds its slot with a `ReaderLease` (VoiceRecorderPure.hpp, host-tested): the
count goes up under the slots mutex in the same critical section as the token lookup and comes down in the
lease's destructor, on every way out. The audit found no leak in the code as it was (the decrement sat after the
last statement and no path returned in between), but that depended on nobody adding an early return; the lease
makes it structural. The body is now sent in 16 KB pieces by `httpd_send()` against a 30 s deadline for the whole
download (`DOWNLOAD_BUDGET_MS`, a floor of 16 KB/s; it normally takes 2 s): `httpd_resp_send()` gave every send the
whole send timeout, so a client taking a few bytes per timeout could hold the web server's only task, and the slot,
for minutes. A failed or cut-off download returns `ESP_FAIL` (the web server closes the connection) and is counted in
`nspanel_voice_http_aborted_total`. On `BOARD_CUSTOM` the web server's `send_wait_timeout` and `recv_wait_timeout` are
3 s (default 5 s): one blocked call holds `/metrics` and everything else for at most that long, and 3 s covers lwIP's
second retransmission (RTO 1.5 s, then 3 s), so a lost tail segment on a healthy link still recovers.

**Announce, and Wi-Fi or MQTT down.** Recording is local: the sink and VoiceCapture's task touch only the slot,
atomics and counters, never Wi-Fi or MQTT (`start()` checks the IP once, before recording), and `stop()` does not
block on the network. Announcing: `MqttManager::publish()` returns `ESP_ERR_NOT_FINISHED` at once when MQTT is
disconnected (it does not queue); when connected, `esp_mqtt_client_publish()` at QoS 1 keeps the message in
esp-mqtt's outbox until it is PUBACKed (resent after a reconnect). So the worker retries itself: `announce_step()`
(pure, tested) backs off 250, 500, 1000, 2000, 2000 ... ms for up to `ANNOUNCE_BUDGET_MS` = 15 s from the first
attempt (leaving 45 s of the URL's 60 for HA to fetch it), then gives up. Retries sit in a table the worker services
every 100 ms, so waiting never stops the automatic stops or another recording's announcement. The payload is rebuilt
on every attempt, so the URL has the IP at that moment and `expires_in` the time left. An attempt checks, in order,
that the token is still served (else fails at once, `ESP_ERR_NOT_FOUND`), that there is an IP (none on the first
attempt fails at once with `ESP_ERR_NOT_ALLOWED`: start() refused without one, so the Wi-Fi went away during the
recording; after a retry it is waited out like MQTT), and `MqttManager::connected()` before calling `publish()`, so
retries do not each log an error and count a publish failure. The result listener fires once per recording: `sent` is
true only if the publish was handed to the client, false with `ESP_ERR_TIMEOUT` when MQTT stayed down for the whole
budget. So the UI shows nothing for up to 15 s after a release with MQTT down, then "Failed";
`nspanel_voice_announce_failures_total` counts each failed recording once. The message, topic and URL format are
unchanged. (A blocking `esp_mqtt_client_publish()` on a half-dead TCP connection can still hold the worker for
esp-mqtt's network timeout, delaying the automatic stops by that long; a release from the UI is not affected.)

**Stacks.** No new gauge: `nspanel_task_stack_unused_bytes{task="voice_rec",mem="internal"}` and
`{task="voice_capture",mem="internal"}` already come from HeapMetrics' scan of every task (the high-water mark, in
bytes never used; a task appears once it exists, i.e. after the first recording). The per-recording VoiceCapture log
line ("stack N bytes unused") stays, and `voice_rec` logs its own after each announcement. The stacks are unchanged
(6144 and 4096). To decide: make 10-20 recordings, some of 15 s, some with MQTT stopped (a retry), then read the two
gauges; they only ever fall. Under about 1 KB unused is too tight; over 3 KB unused on both is room to shrink.

**Tests.** `test/host/voice_recorder` also covers the reader lease (counts, early exit, idempotent, never below
zero), the automatic stops (`auto_stop_reason` and its priority order) and the retry schedule. The worker's use of
them is on-hardware only.
