# Changelog

Releases of this fork (Xboarder56/edge-o-matic-3000). Upstream history is in the git log.

## 2.1.0-rc.6 (2026-11-27): update safety and recordings

**Update safety and recordings release.** Firmware updates are now safer: a bad update rolls back
to the previous version on its own, failed SD installs leave the device working and allow a
retry, and the motor and wired accessories stay stopped while an update runs. SD recordings get
correct CSV columns, regular saves to the card, and a new column marking edges, denials,
orgasms and mode changes.

Includes everything from 2.1.0-rc.2 through rc.5. Clients, plugins and settings work unchanged.

**After updating:** leave the device on for 30 seconds after its first boot on rc.6. That
confirms the update. Turning it off sooner makes it boot the previous version again, and you can
simply install the update again.

### Firmware updates

- A newly installed image stays eligible for rollback until the main loop has run responsively
  for 30 seconds. Confirmation works offline and without an SD card. A reset before confirmation
  lets a rollback-enabled bootloader return to the previous valid image.
- SD installs now reject missing, incomplete, and oversized images, release resources on errors,
  and allow a retry. Successful SD and online installs restart automatically after a
  three-second notice.
- The Update menu now opens with "Stop & check updates". Selecting it pauses plugins, console
  and WebSocket commands, and plugin event delivery. It then stops the motor, switches to
  manual control, sends an immediate halt (`S00`) to wired TS-code accessories, and saves an
  active recording before checking for updates. Motor speed and automatic modes stay locked
  out until you leave the menu, including after a failed install. Bluetooth accessories have no
  common stop acknowledgement, so the menu asks you to stop and disconnect them first.

### Recordings

- SD recordings now include raw pressure immediately after `millis`, so all 12 CSV values
  match their column headings. Classic serial output keeps its existing format.
- Recordings buffer samples in RAM and write and sync them to the SD card every 15 seconds.
  Stopping a recording saves the final partial batch immediately. A power loss between saves
  can lose the current batch.
- SD recordings have a new last column, `events`, that marks the sample where something
  happened: `edge`, `orgasm`, `denial`, `permit`, and mode changes (`mode_manual`,
  `mode_auto`, `mode_plugin`). The first row names the mode the recording started in. Several
  events in one sample are separated by `|`, and the column is empty otherwise. Existing columns
  keep their positions, and classic serial output is unchanged.

### Technical Stuff

- CI: radio settings lock (`tools/radio_lock.py`, `tools/radio-config.lock`). The build and
  release workflows fail if a Wi-Fi/Bluetooth TX power or PHY setting changes, in
  `sdkconfig.defaults` or in the effective build config, or if any source calls runtime
  radio power, country or protocol APIs. The values are Maus-Tec's (`CONFIG_ESP_PHY_MAX_TX_POWER=20`,
  reduced TX power on, PHY calibration as shipped).
- `data_logger_init()` now runs at boot; it registers the recording event markers.

### Verification

- Firmware builds with no new warnings; the radio settings lock passes.
- Host tests of the SD recording writer: every sample written with CSV rows matching the
  headings, the 15-second write and sync cadence, stop and restart, recovery from allocation and
  sync failures, unchanged classic serial output, and event markers on the right rows (markers
  from before a recording are dropped; re-selecting the same mode adds none).
- Host test of the update gate: running work drains before update mode starts, new work is
  refused during it, and a timeout returns the device to normal operation.
- On hardware (EOM3K, Lovense and smart-lock plugins loaded): online update from 2.1.0-rc.5
  through the release feed downloaded and verified the 1.38 MB image; after the restart rc.6
  logged "Updated firmware confirmed after 30 seconds of main-loop activity" and stayed on rc.6
  across a further reset.
- Not yet verified on hardware: rollback after a reset during the first 30 seconds, SD install
  failure and retry, the update-menu motor and accessory stop, and recording event markers.

### Known issues

- With large plugins loaded, online updates need Bluetooth turned off first (Networking, then
  Enable Bluetooth; it takes effect immediately, and you can turn it back on after the update).
  With Bluetooth on, about 40 KB is free and the HTTPS certificate check runs out of
  memory (`PK verify failed`), which shows as no update found. This also applies when updating
  from rc.5 to rc.6. Updating from the SD card is not affected.
- Updating from rc.5: rc.5 stays on "Update successful" after installing and does not restart.
  Restart the device yourself; rc.6 then starts and confirms itself after 30 seconds.

## 2.1.0-rc.5 (2026-11-24): boot fix and remote control

**Boot fix and remote control release.** Fixes devices freezing on the boot screen, gets
Bluetooth running again with several large plugins loaded, and adds WebSocket commands for
remote control and automation: a state snapshot, denial reset, button lock, plugin triggers and
a list of connected Bluetooth devices, plus new events.

Includes everything from 2.1.0-rc.2 through rc.4. Existing clients and plugins work unchanged;
the only visible change for them is that the arousal events now carry the arousal level instead
of 0. Release assets: the app image is now named after the release
(`eom3k-firmware-2.1.0-rc.5.bin`); online updates find it automatically.

### New Features

- **`getState`**: one snapshot of mode, motor, arousal, threshold, denials, button lock, plugin
  control, orgasm detection, radios and free memory, so a client can resync right after
  connecting.
- **`resetDenials`**: resets the denial counter, like "Reset Denial Count" on the device.
- **`setLock`**: locks or unlocks the device buttons and encoder remotely, with a reason. Holding
  BACK+OK for 5 seconds always unlocks.
- **`pluginList` and `pluginEvent`**: list loaded plugins and trigger a feature plugin's
  `remote_*` handler (for example `remote_release`). A plugin decides what is remotely callable by
  naming handlers `remote_*`.
- **`getDevices`**: Bluetooth devices currently connected through a driver plugin.
- **New events** (plugins and `streamEvents`): `lock_changed` (also for the escape hatch),
  `config_changed` (after settings are saved), `edge_start` (orgasm detection starts tracking a
  sustained clench, while armed; registered before but never fired). `streamEvents` also gets
  `device_connect` / `device_disconnect` with the device and plugin name.

### Bugs Fixed

- **Device froze on the boot screen after connecting to Wi-Fi.** Starting Wi-Fi before the HTTP
  server (so plugins load first) made the server's own IP-event handlers register for the first
  time, so the web server was started twice; the second start failed to bind and
  `ESP_ERROR_CHECK` halted the CPU. The duplicate handlers are removed, starting the server is
  idempotent, and a server that fails to start (for example a missing HTTPS certificate) is now
  logged instead of halting the device.
- **Device froze on the boot screen when plugins left too little memory for Bluetooth.** NimBLE
  asserts if it cannot allocate its buffers. With three plugins (Lovense, Coyote and a ~30 KB
  feature plugin) only 16 KB was left. Bluetooth now has room again (see Technical Stuff), and if
  less than 50 KB is free when it would start, it is skipped with a "Bluetooth off: low memory"
  message instead of freezing.
- **The HTTPS private key was printed to the serial log** when HTTPS was enabled. Only its size is
  logged now.
- **Menus redrew at the tick rate in automatic mode.** Speed changes no longer force a full UI
  redraw; the stats page still updates on every change, and Stop/manual commands still redraw.

### Changes

- `orgasm_denial`, `orgasm_permit` and `orgasm_start` carry the arousal level as their value
  (plugins and `streamEvents`), instead of 0.
- Driver plugins also receive `orgasm_denial`, `orgasm_permit`, `orgasm_start` and `edge_start`.

### Technical Stuff

- Boot order: plugins load, then `wifi_manager_init()` (creates the default event loop), then
  `http_server_init()`. Wi-Fi now initializes after plugins, so plugin parsing gets the larger
  heap, as the rc.4 notes intended.
- `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` at boot: the controller runs BLE only,
  so the Classic Bluetooth memory (15 KB) goes back to the heap before plugins load.
- Wi-Fi TX buffers are dynamic (`CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER`, 32, the ESP-IDF default)
  instead of 16 static buffers, which kept ~26 KB reserved at all times.
- Heap on an EOM3K with three plugins: 101 KB free before Wi-Fi, 58 KB before Bluetooth, about
  20 KB after (Bluetooth takes ~38 KB). Boot logs heap before Wi-Fi and before/after Bluetooth.
  Plain plugin HTTP fits; HTTPS (52 KB) does not with this plugin set, and neither does the
  online update (HTTPS): remove large plugins before updating over the internet, or update from
  the SD card or over USB.
- `device_connect` / `device_disconnect` are sent straight to WebSocket clients rather than through
  the event manager: they fire on the Bluetooth host task, where plugin handlers could deadlock.
  Bound devices are kept in a small mutex-protected table for `getDevices`.
- `pluginEvent` handlers run on the web server task, like plugin events triggered by `setMode`.
- The deprecated `CONFIG_TCP_SND_BUF_DEFAULT` / `CONFIG_TCP_WND_DEFAULT` aliases in
  `sdkconfig.defaults` now match the rc.4 values (5760).
- `mt-actions`: `gen_plugin_api.py` also scans event registrations in the actions directory, so
  `plugin-api.json` lists the Bluetooth and HTTP events (16 events). Runtime unchanged.
- New `src/api/state.c`; test script `tools/ws-stress/ws_rc5.py`.

### Verification (on hardware, EOM3K)

| Test | Result |
| --- | --- |
| Boot with Lovense, Coyote and smart-lock plugins, Wi-Fi and Bluetooth on | Boots; Bluetooth starts; ~20 KB free afterwards (rc.4 + boot-order change froze on the boot screen) |
| Boot without an SD card | Boots |
| `ws_rc5.py`: getState, nonce, resetDenials, setLock + lock_changed, pluginList, pluginEvent errors, getDevices, config_changed | 15/15 pass |
| rc.2/rc.3 network tests (API, 30 connect cycles, stalled reader + latency, 10-session flood) | All pass, worst round trip 0.74 s, no resets |
| Online update rc.4 → rc.5 from the device (GitHub feed, renamed image) | Works with only the Lovense plugin loaded (83 KB free). With all three plugins (~20 KB free) the update check fails (`mbedtls_ssl_setup` -0x7F00, out of memory) and the menu hides "Update via Internet" |
| Connecting a Bluetooth device | Not re-tested in this release |

## 2.1.0-rc.4 (2026-11-24): plugin HTTP and Bluetooth

**Plugin HTTP and Bluetooth release.** Plugins can now make HTTP and HTTPS requests (for example
to release an OpenSmartTimer), read characteristics, subscribe to notifications and set up
encrypted links on Bluetooth devices, and use byte and crypto helpers (SHA-256, HMAC-SHA256,
random bytes, hex). Plugins also load much more reliably at boot, and long-lived WebSocket
subscribers survive Wi-Fi/Bluetooth coexistence backpressure without dropping.

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

- **Long-lived WebSocket clients (such as Magical Wardrobe) getting disconnected every few minutes
  (`code=1006`).** `_send_frame_nonblocking` previously closed a client session immediately on any
  non-blocking `EAGAIN` or partial write. With a 2,896-byte default TCP send buffer, a single
  2.4 GHz Wi-Fi retransmission (~1.5 s RTO) or Bluetooth scan coexistence window filled the send
  buffer with ~12 readings frames and kicked healthy subscribers off. Transient `EAGAIN`
  backpressure now drops the superseded broadcast frame cleanly and only disconnects a peer whose
  send buffer stays full continuously for 5 seconds; partial writes finish their remaining bytes
  within 350 ms so WebSocket framing is never corrupted.
- **WebSocket `setMotor` / `setMode` commands (Allow Orgasm, Stop, manual speed) not updating the
  OLED screen or Bluetooth/accessory drivers.** `cmd_edging_set_motor` previously called
  `eom_hal_set_motor_speed` directly without notifying `orgasm_control`, dispatching
  `EVT_SPEED_CHANGE`, or requesting a UI redraw, and `orgasm_control_set_output_mode` / `configSet`
  did not trigger a UI redraw when `arousal == 0`. As a result, remote Allow Orgasm, Stop, and
  manual speed commands never updated Bluetooth driver plugins (such as Lovense) or wired Maus-Bus
  accessories, left the OLED mode/speed bar frozen until arousal changed, and desynchronized
  `orgasm_control`'s cached speed across Auto/Manual transitions. Manual motor changes now go
  through `orgasm_control_set_motor_speed` (clamped to `0..255`), redraw the UI immediately, and
  dispatch `EVT_SPEED_CHANGE` safely from the main control tick.
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

- `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` and `CONFIG_LWIP_TCP_WND_DEFAULT` raised from `2896` (2 MSS) to
  ESP-IDF's standard `5760` (4 MSS); lwIP allocates send pbufs dynamically only while data is
  unacknowledged in flight, so static/idle RAM is unchanged. `websocket_open_fd` enables
  `TCP_NODELAY` on WebSocket client sockets, and `_send_frame_nonblocking` tracks
  `client->stall_since_ms` (`WS_CLIENT_STALL_TIMEOUT_MS = 5000`) and completes partial frame tails
  via `select()` (`WS_PARTIAL_SEND_TIMEOUT_MS = 350`).
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
