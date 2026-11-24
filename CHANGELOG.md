# Changelog

Releases of this fork (Xboarder56/edge-o-matic-3000). Upstream history is in the git log.

## 2.1.0-rc.4 (2026-11-24): plugin HTTP and Bluetooth

**Plugin HTTP and Bluetooth release.** Plugins can now make HTTP and HTTPS requests (for example
to release an OpenSmartTimer), read characteristics, subscribe to notifications and set up
encrypted links on Bluetooth devices, and use byte and crypto helpers (SHA-256, HMAC-SHA256,
random bytes, hex). Plugins also load much more reliably at boot.

Includes the 2.1.0-rc.2 network stability fix and the 2.1.0-rc.3 API fixes. Existing plugins
work unchanged: everything new is opt-in through new permissions (`net:http`, `ble:read`,
`ble:notify`, `ble:secure`). Plugin author reference: `doc/PluginHostAPI.md`.

### New Features

- **HTTP and HTTPS requests from plugins** (`net:http`). `http_request` returns at once and the
  result arrives as the `http_response` event; JSON responses can be read by path
  (`http_json_string`, `http_json_int`). Custom headers, JSON or form bodies, and an option to skip
  TLS certificate checks for self-signed devices. One request at a time.
- **More Bluetooth for driver plugins.** Write to a chosen characteristic, read characteristics,
  subscribe to notifications or indications, encrypt and bond (LE Secure Connections, "Just
  Works"; bonds are kept across restarts), and negotiate a larger packet size.
- **Byte and crypto helpers** for building and checking binary frames: SHA-256, HMAC-SHA256,
  random bytes, hex conversion, and little/big-endian integer packing.

### Bugs Fixed

- **Plugins failing to load at boot.** Plugins were loaded after Wi-Fi and Bluetooth started,
  when memory is lowest; parsing a plugin briefly needs several times its file size, so with a
  few plugins installed the device fell to under 100 bytes free and later plugins failed with a
  "Failed to parse plugin JSON" error. Plugins now load before the network starts (lowest point
  during boot ~22 KB instead of ~0).
- **Plugin settings stuck at a limit.** If a plugin's default value lay outside the range it
  declares, the first change snapped the setting to the limit and it could never get back. For
  the Lovense driver (`max_level` default 20, declared max 5) the vibration level could not be
  raised above 5 again. The setting's range now includes the plugin's default and the saved value.

### Technical Stuff

- New `src/actions/http.c`, `src/actions/bytes.c`; Bluetooth extensions in `src/actions/ble.c`
  and `src/drivers/plugin_driver.c`. NimBLE callbacks only queue results; plugins get them on the
  plugin task like every other driver event.
- HTTP requests run on a short-lived worker task at the lowest priority, so a TLS handshake never
  delays the main loop or Bluetooth drivers. Plain HTTP never follows redirects (one to HTTPS
  could exceed the memory that was checked); HTTPS follows up to 3.
- Memory, measured on an EOM3K: plain HTTP ~15 KB, HTTPS to an internet host (GitHub Pages) more
  than 42 KB. Requests are refused unless 12 KB (HTTP) or 52 KB plus a 20 KB block (HTTPS) is
  free, so **HTTPS only runs with one small plugin loaded** (the Lovense driver alone leaves
  ~57 KB); plain HTTP is the practical choice for devices on your network. HTTPS also waits while
  a Bluetooth device is connecting.
- mbedTLS dynamic buffers were tried and left off: they lower the HTTPS handshake peak (~39 KB),
  but re-allocating the 16 KB receive buffer for every record fragmented the heap and the
  1.4 MB online update failed partway through. TLS keeps fixed buffers, as in upstream.
- Bond storage (`ble_store_config_init`) is now enabled. Encryption is only started when a plugin
  calls `ble_secure`, so other devices connect exactly as before.
- Each plugin load logs the free heap afterward.
- `tools/plugin-tests/` holds the test plugins and scripts used below.

### Verification (on hardware, EOM3K)

| Test | Result |
| --- | --- |
| Plain HTTP: GET, POST with JSON body and header, 404 error body, 6 KB body (capped), busy/invalid arguments | All correct; 30/30 requests in a loop across two hosts |
| HTTPS: GitHub Pages verified (with redirect), self-signed with verification on / off | 200 and JSON parsed / rejected (`-0x2700`) / 200 (development build with dynamic TLS buffers; with the final fixed buffers HTTPS needs 52 KB free and was not re-run) |
| Online update over the internet (test feed, then the real `firmware.bin` from GitHub) | Downloaded 1.38 MB, verified, rebooted into the new slot |
| Update from SD card (`update.bin`) | Works |
| SHA-256, HMAC-SHA256 (RFC 4231 #1), byte packing | Match reference values |
| DG-Lab Coyote 3 (zero output): write to `150A`, MTU, subscribe `150B`, read battery, B1 notifications, 30 B0 frames at 100 ms | All working |
| Lovense Hush with the unchanged driver | Works as before |
| rc.2/rc.3 network tests (API, 30 connect cycles, stalled reader, 10-session flood) | All pass |
| Boot with 4 plugins (Lovense, smoke test, two test plugins): lowest free heap | ~22 KB (76 bytes when loaded after the network, as before) |

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
