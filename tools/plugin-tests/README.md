# Plugin host function tests

On-device checks for the HTTP, Bluetooth and byte/crypto plugin functions added in 2.1.0-rc.4
(see `doc/PluginHostAPI.md`). Requires Python 3 with `websockets` (and `pyserial` for SD uploads).

| File | Purpose |
| --- | --- |
| `gen_net_test.py <mac-ip>` | Writes `net-test.json`, a feature plugin that runs one HTTP/bytes test per `setMode` |
| `servers.py` | Test servers on this machine: HTTP :8765 (`/json`, `/echo`, `/slow`, `/big`, 404) and self-signed HTTPS :8766. Needs `cert.pem`/`key.pem` (see below) |
| `drive_steps.py <eom-ip> <n> [gap-s]` | Sends `{"setMode": "manual"}` n times, one test step each |
| `gen_coyote_test.py` | Writes `coyote-test.min.json`, a `ble_driver` for a DG-Lab Coyote 3 that exercises the Bluetooth functions with **zero output** (soft limits 0, strength 0) |
| `sd_session.py`, `verify_put.py` | Upload files to the SD card over USB and check them byte for byte |

## Running

```sh
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
  -keyout key.pem -out cert.pem -days 7 -subj "/CN=eom-test"
python3 servers.py &
python3 gen_net_test.py <this-machine-ip>    # LAN address the EOM can reach
python3 -c "import json;d=json.load(open('net-test.json'));open('net-test.min.json','w').write(json.dumps(d,separators=(',',':')))"
```

Copy `net-test.min.json` to the SD card as `/plugins/net-test.json`, restart, wait at least 20 s
after boot (the plugin ignores the boot-time `mode_set`), then:

```sh
python3 drive_steps.py <eom-ip> 11 8         # steps 1-11, then any more for the HTTP loop
```

Results are printed to the serial log (`[net-test]` and `actions:http` lines). Each finished
request logs its status, size, the lowest free heap while it ran, and unused worker stack.

For the Coyote test, copy `coyote-test.min.json` as `/plugins/coyote-test.json`, restart, and
connect the Coyote from the Bluetooth menu. Remove it afterward: it claims any `47L121...` device.

## Copying files without removing the SD card

The serial console is disabled in normal builds (it costs an 8 KB task). For a test-only build,
uncomment `console_ready();` in `src/main.c`, flash it, then:

```sh
EOM_PORT=/dev/cu.usbserial-XXXX python3 sd_session.py upload.log 1 "put:net-test.min.json:/plugins/net-test.json" "cmd:ls /plugins"
python3 verify_put.py upload.log net-test.min.json
```

Paths are relative to the SD card root. Opening the port resets the board; the script waits for
`READY.`. Uploads are throttled because the console has no flow control and echoes every byte.
Flash the normal build again afterward; files stay on the card.
