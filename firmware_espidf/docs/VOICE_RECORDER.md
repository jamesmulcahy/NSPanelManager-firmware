# Voice recorder (custom_pcb) — phase 2a design

Hold-to-talk on the panel → a WAV in PSRAM → `http://<panel IP>/rec/<token>.wav` → an MQTT message → a
Home Assistant automation calls `script.broadcast_voice_prompted` with the URL, and the speakers fetch and
play it. No STT, no TTS, no models (GSC + WebRTC NS run without the `model` partition).

Interface: [`lib/VoiceRecorder/VoiceRecorder.hpp`](../lib/VoiceRecorder/VoiceRecorder.hpp) (declarations
only; phase 2b writes the .cpp). This file is the reasoning and the parts that are not C++.

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

- **Allocation:** on the first `start()`, `SLOTS = 2` buffers of `SLOT_BYTES` (960 044 B) each with
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`: **1.92 MB of the board's 32 MB PSRAM**, never freed. Never
  freeing is what makes use-after-free impossible; the rules below only prevent a buffer being
  *overwritten* while it is being sent.
- **Max length 30 s** (960 KB per slot): an announcement, not a dictation; 30 s keeps two slots under
  2 MB and a full download under ~1 s on Wi-Fi. Longer is a constant change (`MAX_SECONDS`), ~32 KB/s.
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
| Lifetime | `TOKEN_TTL_S = 300` s from `stop()`, or until its slot is reused, whichever is first |
| Use | **Multi-use** within its lifetime: HA, Sonos and Cast fetch the URL themselves, often more than once (a HEAD, then ranged GETs, and one fetch per speaker) |
| Unknown token / bad form | `404 Not Found` |
| Known but expired | `410 Gone` (the slot still holds it, past `expires_us`); after the slot is reused it's 404 |
| Comparison | constant-time over the 32 chars |

Not single-use because it would break multi-speaker groups and players that probe first; the 128-bit
token on the LAN plus the 5 min expiry is the protection.

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
- The web server has one task, so a 960 KB send holds other requests for ~1 s. Accepted.

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
  "expires_in": 300,
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
| `{"command":"voice_record","action":"start","seconds":5}` | record 5 s (1–30), then stop and announce |
| `{"command":"voice_record","action":"start"}` | record until `stop`/`cancel` (or the 30 s limit) |
| `{"command":"voice_record","action":"stop"}` | `stop_and_send()` |
| `{"command":"voice_record","action":"cancel"}` | `cancel()` |

```sh
mosquitto_pub -t 'nspanel/C0:49:EF:60:AD:68/command' -m '{"command":"voice_record","action":"start","seconds":5}'
mosquitto_sub -v -t 'nspanel/+/voice/recording'
```

## 7. Home Assistant

```yaml
automation:
  - alias: "NSPanel voice broadcast"
    mode: queued
    max: 5
    trigger:
      - platform: mqtt
        topic: "nspanel/+/voice/recording"
    condition:
      - "{{ trigger.payload_json.url is defined }}"
    action:
      - service: script.broadcast_voice_prompted
        data:
          # The script's own field names; adjust to match it.
          audio_url: "{{ trigger.payload_json.url }}"
          source: "{{ trigger.payload_json.name }}"
          # e.g. exclude the speaker in the panel's room:
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
| `nspanel_voice_sink_overruns_total` | sink calls dropped because the slot was full or no slot was RECORDING |

## 9. Open questions for the user

1. **Max length**: 30 s here. Longer is cheap (32 KB per second per slot), but is anything over 30 s wanted?
2. **Repeat fetches**: the URL is multi-use for 5 minutes. OK, or should it be shorter / single-use (breaks speaker groups)?
3. **At the limit**: auto-send (this design) or stop and wait for the user to release/cancel?
4. **Retain**: not retained, so a recording made while HA is restarting is lost. Acceptable?
