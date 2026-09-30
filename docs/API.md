# API reference

This page covers two APIs:

1. **Plugin API**: `PhoneLink.asi` inside the game, reached on localhost. Studio uses it, and so can your own tools.
2. **Signaling protocol**: between the call page (the friend's browser) and Studio.

---

## 1. Plugin API

The defaults are `http://127.0.0.1:8765` for HTTP and `ws://127.0.0.1:8766` for WebSocket; both are set in `PhoneLink.ini`. If `Token` is set, authenticate in one of these ways:

- the `Authorization: Bearer <token>` header
- the `X-Api-Token` header
- a `?token=` query parameter

Commands sent while the player isn't in the game world (for example on the loading screen) are queued, and run once the player is back.

### HTTP

| Method | Path | Body | Result |
|---|---|---|---|
| GET | `/status` | none | `{version, in_game, call_state, call_id}` |
| POST | `/sms` | `{from, text, subject?, icon?, flash?}` | `202 {ok}` |
| POST | `/call` | `{from, icon?, text?, audio_base64? \| audio_path?, ring_timeout?, id?}` | `202 {ok, id}` |
| POST | `/hangup` | `{id?, reason?}` | `202 {ok}` |
| POST | `/subtitle` | `{id?, text, duration_ms?}` | `202 {ok}` |
| POST | `/connect` | `{id}` | `202 {ok}` |
| POST | `/contacts` | `{contacts: [{id, name}]}` | `202 {ok}` |

**`call_state` values:** `idle`, `ringing`, `dialing`, `connecting`, `active`.

**Where `/call` gets its audio,** checked in this order:
1. `audio_base64`: a wav/mp3/flac file, base64-encoded
2. `audio_path`: a local audio file
3. `text` alone: spoken with Windows text-to-speech

When there's audio, `text` is also shown as subtitles. A **streamed** call (below) is only available over WebSocket.

**`icon`** is a feed picture name such as `CHAR_LESTER`, `CHAR_FRANKLIN` or `CHAR_DEFAULT`.

```powershell
curl -X POST http://127.0.0.1:8765/sms  -H "Content-Type: application/json" -d '{"from":"Lester","text":"Meet me at the docks.","icon":"CHAR_LESTER"}'
curl -X POST http://127.0.0.1:8765/call -H "Content-Type: application/json" -d '{"from":"Lester","text":"I have a job for you."}'
```

### WebSocket

Send JSON objects with a `type`: `sms`, `call`, `hangup`, `subtitle`, `connect`, `contacts` or `status`. The fields are the same as for HTTP. Each request gets an `{"event":"ack","ok":…}` reply, which echoes your `req_id` if you sent one.

**Events broadcast to every client:**

| Event | Fields | When |
|---|---|---|
| `hello` | `version, in_game, call_state` | on connect |
| `sms_shown` | `from` | a text was shown |
| `call_ringing` | `id, from` | an incoming call is ringing in-game |
| `call_dialing` | `id, to` | the player is calling out (see *Outgoing calls*) |
| `call_answered` | `id` | the player pressed Y |
| `call_active` | `id`, and for streamed calls `sample_rate, channels, mic, mic_sample_rate` | audio is live |
| `call_declined` / `call_missed` | `id` | |
| `call_rejected` | `id, reason:"busy"` | another call is already in progress |
| `call_ended` | `id, reason, duration_ms?` | always sent last for every call |
| `dial` | `contact, name` | the player chose a contact in the F7 menu |

**`call_ended` reasons:** `completed`, `declined`, `missed`, `player_hung_up`, `remote_hung_up`, `media_failed`, `player_unavailable`, and any `reason` passed to `hangup`.

### Streamed (live) calls

```json
{"type":"call","from":"Ali","stream":true,"sample_rate":48000,"channels":1,"mic":true,"mic_sample_rate":48000}
```

1. Once `call_active` arrives, send **binary** frames: signed 16-bit little-endian PCM at your `sample_rate`/`channels`. 20 ms frames are recommended. The plugin keeps about 100 ms of buffer to absorb jitter, and drops audio that falls more than 2 s behind.
2. With `mic: true`, you receive binary frames of the player's microphone: 16-bit mono PCM at `mic_sample_rate`.
3. `{"type":"subtitle","text":"…"}` shows a subtitle line during the call.
4. The call ends when any of these happens:
   - you send `hangup`
   - you close the socket that started the call
   - the player presses **N**

### Outgoing calls (player calls someone)

1. Send the contact list: `{"type":"contacts","contacts":[{"id":"a1","name":"Ali"}]}`. It's shown in the F7 menu.
2. When the player picks a contact, the plugin broadcasts `{"event":"dial","contact":"a1","name":"Ali"}`.
3. Start the call with `"outgoing": true` added to the streamed-call message above. The game shows **CALLING…** and plays ringback, and doesn't ring the player.
4. When the other side picks up, send `{"type":"connect","id":"<call id>"}`. The call becomes `active`.
5. If they don't pick up, send `{"type":"hangup","id":…,"reason":"declined"|"no_answer"|"unavailable"}`. The game shows a matching *Call failed* notification.

See `examples/phonelink_client.py` for a small client that places a streamed call and records the mic.

---

## 2. Signaling protocol (call page ↔ Studio)

The call page is served at `http://127.0.0.1:8771/` (and publicly through the tunnel). It uses one WebSocket at `/signal`. Audio flows over WebRTC. Studio answers offers with Opus at 48 kHz and waits until all ICE candidates are gathered, so there's no trickle ICE.

**Page → Studio:**

| Message | Meaning |
|---|---|
| `{type:"hello", code, name}` | Must be first. A wrong `code` gets `denied`; 5 failures from one IP within 10 minutes get `rate_limited`. |
| `{type:"profile", name}` | Set or change the display name. Friends only appear in the F7 contacts list once they have a name. |
| `{type:"status"}` | Ask for availability |
| `{type:"call", sdp, name}` | Call the player. `sdp` is a WebRTC offer. |
| `{type:"accept", sdp, name}` / `{type:"decline"}` | Answer or reject an `incoming` call |
| `{type:"hangup"}` | End the current call |
| `{type:"sms", text, name}` | Text the player. At most 300 characters and one text every 1.5 s. |

**Studio → page:**

| Message | Meaning |
|---|---|
| `{type:"welcome", host, status, iceServers}` | Accepted. `status` is `available`, `busy` or `offline`. |
| `{type:"denied", reason}` | `bad_code` or `rate_limited` |
| `{type:"status", status}` | Availability |
| `{type:"answer", sdp}` | The WebRTC answer |
| `{type:"ringing"}` | The player's phone is ringing |
| `{type:"incoming", from}` | The player is calling this page |
| `{type:"connected"}` | The call is live |
| `{type:"ended", reason}` | `hung_up`, `host_hung_up`, `declined`, `no_answer`, `busy`, `offline`, `cancelled`, `caller_left`, `connection_failed`, `host_unavailable` or `failed` |
| `{type:"sms_result", ok, reason?}` | `reason` is `offline` or `slow_down` |

**Control panel API** (private, `127.0.0.1:8770`, used by `static/control.html`):
- `GET /api/status`
- `GET /api/events` (server-sent events)
- `POST /api/sms`
- `POST /api/test-call`
- `POST /api/hangup`
- `POST /api/settings/calls`
- `POST /api/settings/plugin`
- `GET /api/gta`
- `POST /api/gta/install`
- `GET /api/log`
