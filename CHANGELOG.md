# Changelog

Releases of this fork (Xboarder56/edge-o-matic-3000). Upstream history is in the git log.

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
