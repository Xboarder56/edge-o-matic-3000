# Changelog

Releases of this fork (Xboarder56/edge-o-matic-3000). Upstream history is in the git log.

## 2.1.0-rc.3 (2026-11-23): API and update fixes

**API and update fixes.** Standard WebSocket keepalive and close now work, clients can subscribe
to device events (denials, edges allowed, orgasm detected, mode changes), `streamReadings: false`
works, and online updates now come from this fork instead of Maus-Tec.

Includes the 2.1.0-rc.2 network stability fix. Existing clients keep working unchanged: the new
features are opt-in, and existing readings fields are unchanged.

### New Features

- **Device events over the WebSocket API.** Send `{"streamEvents": true}` to receive events as
  they happen, using the same names as plugin events:
  `{"event": {"name": "orgasm_denial", "value": 0, "millis": 123456}}`.
  Events: `orgasm_denial`, `orgasm_permit`, `orgasm_start`, `mode_set` (which also carries
  `runMode`). Send `{"streamEvents": false}` to stop. Clients that don't subscribe see no change.
- **Denial count in readings.** Readings now include `denials`, the count shown on the device's
  edging screen (since boot or the last "Reset Denial Count").

### Bugs Fixed

- **WebSocket pings were never answered.** Clients that send standard keepalive pings (most
  WebSocket libraries do by default) timed out and disconnected, so integrations had to turn
  pings off and guess whether the device was still there. The device now replies to pings and
  completes the close handshake, so closing a connection takes milliseconds instead of the
  client's close timeout.
- **`{"streamReadings": false}` did nothing.** Any value turned streaming on. `false` now stops
  readings (system status updates continue).
- **Online updates pointed at Maus-Tec's service.** The Update menu sent the device serial number
  to Maus-Tec, and a future official release could have replaced this fork's firmware. Online
  updates now use this fork's release feed, and the serial number is no longer sent. Devices
  still on the old default address are switched automatically; a custom `remote_update_url` is
  kept. Set `remote_update_url` to `""` in `config.json` to turn online update checks off.
- **Sending `setMode` with a non-text value could crash the device.** It is now rejected with an
  error.
- **Online update checks leaked a connection each time.** The HTTP client is now cleaned up.

### Technical Stuff

- The WebSocket URI handler no longer requests control frames
  (`handle_ws_control_frames = false`), so esp_http_server replies PONG to PING and CLOSE to
  CLOSE (RFC 6455).
- New broadcast flag `WS_BROADCAST_EVENTS`. `api_broadcast_events_init()` registers event-manager
  handlers that queue events through the rc.2 non-blocking broadcast path. Pending broadcast work
  is capped at 8 (was 4) so events aren't crowded out by readings.
- Update feed: `remote_update_url` defaults to `https://xboarder56.github.io/edge-o-matic-3000`
  (GitHub Pages, published by the release workflow): `version.txt` holds the latest version, and
  the index is JSON with the release's `firmware.bin` as the `image` asset. Feed requests verify
  TLS certificates and ignore redirect response bodies. Config file version 5 (migration
  `migrate_to_5`) replaces only the exact old default.
- `tools/ws-stress/ws_rc3.py` checks the new API behavior against a live device.

### Verification (on hardware, EOM3K)

| Test | Result |
| --- | --- |
| Ping answered with pong | 15 ms |
| Close handshake | 17 ms (2.1.0-rc.2: client close timeout) |
| Client keepalive pings every 1 s for 6 s | Connection stays up |
| `streamReadings` true / false | 43 readings in 3 s / 0 after stop |
| `streamEvents`: subscribed, unsubscribed, after `false` | Events delivered / none / none |
| `setMode` with number, null, object | Rejected (`errno 1`), device keeps running |
| rc.2 regression tests (30 connect cycles, stalled reader, 10-session flood) | All pass, worst round trip 0.37 s |
| Config migration from rc.2 | Old Maus-Tec URL replaced |

## 2.1.0-rc.2 (2026-11-23): network stability fix

**Network stability fix release.** This fixes the EOM3000 locking up and becoming unreachable when
too many web or app connections are opened, and the device freezing (screen, buttons, arousal
sensing and motor control) while a connected client is slow or has gone to sleep.

Based on 2.1.0-rc.1 and built entirely from this fork (reconstructed `mt-actions` runtime plus
mirrored dependencies). Everything else behaves the same as 2.1.0-rc.1.

### Bugs Fixed

- **Device stops accepting connections after a few uses (network stability fix).** Every web or
  app connection that closed leaked a network socket. With only 6 sockets available, the device
  refused all new connections after about two connect/disconnect cycles and stayed unreachable
  (also breaking update checks) until it was power-cycled. Sockets are now released on close.
- **Device freezes while a client is connected (network stability fix).** Sensor readings were
  streamed to clients from the main control loop using blocking sends. A client that stopped
  reading (phone asleep, browser tab in background, flaky Wi-Fi) froze the main loop for up to
  5 seconds per update, which stalled the UI, buttons, arousal detection and motor control.
  Broadcasts now run on the web server's own task and never block the main loop. A client that
  can't keep up is disconnected instead of waited on.
- **Possible crash when a client disconnects during a broadcast.** The client list was changed
  by the web server while the main loop was iterating over it. It is now only accessed from one
  task.

### Technical Stuff

- `websocket_close_fd` now calls `close()`. When an httpd `close_fn` is installed,
  esp_http_server does not close the socket itself. Previously `accept()` failed with
  `ENFILE (23)`.
- `websocket_broadcast` serializes the payload and queues it with `httpd_queue_work`. Frames go
  out as a single non-blocking send (`MSG_DONTWAIT`). On a short or failed send the session is
  closed with `httpd_sess_trigger_close`. Readings broadcasts are coalesced (one pending at most),
  and pending broadcast work is capped at 4.
- TCP keepalive on client sockets (10 s idle, 5 s interval, 3 probes) frees slots held by peers
  that vanished without closing.
- `lru_purge_enable` is on, so a new client replaces the least recently used session when all
  slots are full, instead of being refused.
- `CONFIG_LWIP_MAX_SOCKETS` raised from 6 to 10 and `max_open_sockets` from 3 to 4, leaving sockets
  free for outbound connections (OTA, HTTP client). Handler send/recv timeouts are bounded
  (3 s / 5 s).

### Verification (on hardware, EOM3K, against 2.1.0-rc.1)

| Test | 2.1.0-rc.1 | 2.1.0-rc.2 |
| --- | --- | --- |
| Repeated connect, command, disconnect | Fails from the 3rd connection (`accept` ENFILE) | 30/30 succeed |
| Client subscribes to readings, then stops reading (40 s) | Broadcasts block ~6 s each; device and other clients stall | Stalled client disconnected immediately; other clients' worst round trip 0.27 s |
| 10 simultaneous sessions dropped abruptly | n/a (already wedged) | No lockup; responsive within 0.3 s |
| Sessions opened / closed over the run | Sockets leaked on every close | 91 / 91 |
| Free heap before / after | n/a | 44.2 KB / 43.6 KB (no leak) |

## 2.1.0-rc.1 (fork baseline)

Upstream MausTec 2.1.0-rc.1, built from this fork with the reconstructed `mt-actions` v1.3.2
(Xboarder56/mt-actions) and self-hosted dependency mirrors. No functional changes from upstream.
