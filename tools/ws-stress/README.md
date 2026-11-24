# WebSocket stress tests

Regression tests for the network stability fixes in 2.1.0-rc.2. They run against a live device
over Wi-Fi. Requires Python 3 and `pip install websockets`.

```sh
python3 ws_probe.py <device-ip>          # one command round trip
python3 ws_cycle.py <device-ip> 30       # A: connect/command/close cycles (socket leak)
python3 ws_stall.py <device-ip> 40 &     # B: subscriber that stops reading...
python3 ws_latency.py <device-ip> 38     #    ...while measuring other clients' latency
python3 ws_flood.py <device-ip> 10       # C: more sessions than slots, dropped abruptly
python3 ws_rc3.py <device-ip>            # rc.3 API: ping/pong, close, streamReadings false,
                                         #   streamEvents, denials, setMode validation
```

Expected on 2.1.0-rc.2 or later: every cycle succeeds, no latency probe fails during the
stalled-reader test (round trips well under 1 s), and the device stays reachable after the flood.
On 2.1.0-rc.1, `ws_cycle.py` fails from the third connection (`accept` returns ENFILE) and
`ws_stall.py` freezes the device's main loop for ~5 s per readings broadcast.
