# Plugin host API additions (2.1.0-rc.4)

This fork adds host functions for plugins: HTTP(S) requests, more Bluetooth (GATT) access for
`ble_driver` plugins, and byte/crypto helpers. They are additions only. Existing plugins load and
behave exactly as before, and a plugin gets none of this unless it declares the permission and
calls the function. The generated `plugin-api.json` (attached to each release) lists every function
with its arguments.

Calls use the usual mt-actions forms: `{"fn": arg}` or `{"fn": [arg1, arg2]}`, with the result in
`$_` or captured with `"to": "$var"`. Functions without arguments are called as `{"fn": []}`.
Byte arrays are declared as plugin variables, e.g. `"variables": {"frame[32]": 0}`.

## HTTP and HTTPS (`net:http`)

Requests never block. `http_request` returns at once, and the result arrives later as the
`http_response` event, delivered only to the plugin (or BLE driver instance) that made the request.
**The response is available only inside that handler**; it is freed when the handler returns.

| Function | Returns |
| --- | --- |
| `http_request(method, url[, body[, flags]])` | Request ID > 0, or `-1` busy, `-2` invalid method/URL, `-3` not enough memory |
| `http_set_header(name, value)` | `0`, or `-1` (up to 4 headers on the next request) |
| `http_busy()` | `1` while a request is running or waiting to be delivered |
| `http_response_id()` | ID of the response being delivered |
| `http_response_body()` | Body as text (capped at 4096 bytes) |
| `http_response_length()` | Bytes received, or `-1` if the body was cut off at 4096 |
| `http_response_copy(array)` | Copies the body into a byte array |
| `http_json_has(path)`, `http_json_string(path)`, `http_json_int(path)` | Read the JSON body by dotted path, e.g. `state` or `items.0.name` |

Event `http_response` (permission `net:http`): payload is the HTTP status, or `-1` connection
failed, `-2` timed out, `-3` out of memory.

Details:

* Methods: GET, POST, PUT, PATCH, DELETE, HEAD. URL up to 255 characters, body up to 1024 bytes,
  10 s timeout.
* A body that looks like JSON is sent as `application/json`, otherwise as a form, unless you set
  `Content-Type` with `http_set_header`.
* `flags = 1` skips TLS certificate verification (self-signed devices). Without it, HTTPS
  servers are verified against the built-in CA bundle.
* HTTPS follows up to 3 redirects. Plain HTTP never follows redirects (one could lead to HTTPS,
  which needs far more memory than was checked for); the plugin gets the 3xx status. Error bodies
  (4xx/5xx) are delivered, so a JSON error code can be read.
* **One request at a time on the whole device.**

### Memory

Measured on an EOM3K:

| Request | Peak heap |
| --- | --- |
| Plain HTTP | ~15 KB |
| HTTPS to a LAN device (ECDSA certificate) | ~28 KB |
| HTTPS to an internet host (GitHub Pages, verified) | ~39 KB |

The device typically has 30-45 KB free, depending on the loaded plugins (each costs roughly 2-3
times its `plugin.json` size while loaded). A request is refused with `-3` unless at least 12 KB
(HTTP) or 44 KB plus a 20 KB contiguous block (HTTPS) is free. In practice, **HTTPS works with one
or two small plugins loaded; prefer plain HTTP for devices on your own network.** HTTPS requests
also wait while a Bluetooth device is being connected.

### Example: release an OpenSmartTimer

Use the timer's IP address (a DHCP reservation helps). mDNS lookups are enabled in the firmware,
so `smarttimer.local` may also work, but that has not been tested on a device.

```json
"variables": {"rid": 0, "state": ""},
"events": {
  "orgasm_denial": {"actions": [
    {"http_request": ["POST", "http://192.168.1.42/api/v1/timer/release", "{}"], "to": "$rid"}
  ]},
  "http_response": {"args": ["status"], "actions": [
    {"http_json_string": "state", "to": "$state"},
    {"log": "$state"}
  ]}
}
```

## Bluetooth (`ble_driver` plugins)

Writes still go to the first writable characteristic found unless the plugin calls
`ble_use_characteristic`. Everything below applies only to the calling plugin's own connection.

| Function | Permission | Returns |
| --- | --- | --- |
| `ble_use_characteristic(uuid)` | `ble:write` | `1` found and writable, else `0` |
| `ble_tx_busy()` | `ble:write` | `1` while a write is queued or awaiting its response |
| `ble_request_mtu()` | `ble:write` | `0` started (result: `ble_mtu` event) |
| `ble_mtu()` | `ble:write` | Current ATT MTU (23 until negotiated; the EOM offers 256) |
| `ble_read(uuid)` | `ble:read` | `0` started (result: `ble_read` event), `-1` busy, `-2` not found, `-3` not readable |
| `ble_subscribe(uuid[, mode])` | `ble:notify` | Same codes; mode `1` notify (default), `2` indicate, `0` stop |
| `ble_secure()` | `ble:secure` | `0` started (result: `ble_secured` event) |
| `ble_is_secure()` | `ble:secure` | `1` if the link is encrypted |
| `ble_rx_length()`, `ble_rx_byte(i)`, `ble_rx_copy(array)`, `ble_rx_string()`, `ble_rx_uuid()` | none | Data being delivered (inside `ble_read` / `ble_notify`) |

| Event | Permission | Payload |
| --- | --- | --- |
| `ble_read` | `ble:read` | Bytes read (max 128), or negative error |
| `ble_notify` | `ble:notify` | Bytes received (max 128) |
| `ble_subscribed` | `ble:notify` | `0`, or negative error |
| `ble_secured` | `ble:secure` | `0` when encrypted, or negative error |
| `ble_mtu` | `ble:write` | Negotiated MTU, or negative error |

Details:

* UUIDs: `"150A"`, `"0x150A"`, 8 hex digits, or a full 36-character UUID. Bluetooth-base UUIDs
  match in either spelling.
* One read or subscribe at a time per connection. A new `ble_write` replaces one that has not
  been sent yet; wait for `ble_tx_busy()` to be `0` before the next frame of a sequence.
* `ble_secure` bonds with LE Secure Connections, "Just Works" (the EOM cannot show or enter a
  passkey). Bonds are stored and survive restarts (up to 3). If the device forgot the bond, the
  EOM drops its copy so the next `ble_secure` pairs again.
* The EOM connects to at most 3 Bluetooth devices at once.

## Bytes and crypto (no permission)

| Function | Notes |
| --- | --- |
| `sha256(data, out)` | 32-byte digest into `out` (capacity >= 32) |
| `hmac_sha256(key, data, out)` | Key up to 64 bytes, data up to 128 bytes |
| `random_bytes(out, count)` | Hardware RNG |
| `hex_to_bytes(hex, out)` / `bytes_to_hex(data)` | Spaces, `:` and `-` are ignored in hex |
| `bytes_append(out, data)` | Append a string or array; `-1` if it does not fit |
| `bytes_append_int(out, value, size)` | 1, 2 or 4 bytes little-endian; negative size = big-endian |
| `bytes_read_int(data, offset, size)` | Same sizes; unsigned |
| `bytes_clear(out)` | Length 0 |
| `bytes_equal(a, b, n)` | Constant-time compare of the first `n` bytes |

`data` arguments accept a string or a byte array (up to 128 bytes); `out` arguments are written
in place, up to the array's declared capacity.

## Testing

`tools/plugin-tests/` has the test plugins and scripts used to validate these functions on a
device.
