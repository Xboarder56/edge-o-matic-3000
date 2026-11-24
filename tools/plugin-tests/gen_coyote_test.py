"""Coyote 3 BLE test driver: zero-output only (soft limits 0, strength 0)."""
import json

def log(*parts):
    # concat takes strings only: convert variables with to_string first.
    acts, out = [], []
    for i, p in enumerate(parts):
        if isinstance(p, str) and p.startswith("$"):
            acts.append({"to_string": p, "to": "$s%d" % i}); out.append("$s%d" % i)
        else:
            out.append(p)
    return acts + [{"concat": out}, {"log": "$_"}]

def b(arr, *vals):
    return [{"bytes_append_int": [arr, v, 1]} for v in vals]

zero_b0 = ([{"bytes_clear": "$pkt"}] + b("$pkt", 0xB0) +
           [{"bytes_append_int": ["$pkt", "$hdr", 1]}] + b("$pkt", 0, 0) +   # strengths A, B = 0
           b("$pkt", 10, 10, 10, 10, 0, 0, 0, 0, 10, 10, 10, 10, 0, 0, 0, 0))  # waves: freq 10, strength 0

plugin = {
    "name": "coyote-test", "display_name": "Coyote Test", "version": "0.1.0",
    "sdk_version": "~> 1.1.0", "type": "ble_driver",
    "description": "Firmware BLE extension checks against a DG-Lab Coyote 3 (zero output).",
    "author": "test", "license": "MIT", "platforms": ["eom3k ~> 2.1.0"],
    "permissions": ["ble:write", "ble:read", "ble:notify"],
    "match": {"ble_name_prefix": "47L121"},
    "variables": {"s0": "", "s1": "", "s2": "", "s3": "", "s4": "", "s5": "", "s6": "", "s7": "", "s8": "", "s9": "", "started": 0, "last": 0, "sent": 0, "hdr": 15, "tmp": 0, "tmp2": 0, "tmp3": 0,
                  "pkt[20]": 0, "bf[7]": 0, "rx[32]": 0},
    "functions": {},
    "events": {
        "connect": {"actions":
            log("coyote-test: connected") +
            [{"ble_use_characteristic": "150A", "to": "$tmp"}] + log("use_characteristic(150A) -> ", "$tmp") +
            [{"ble_use_characteristic": "ffff", "to": "$tmp"}] + log("use_characteristic(ffff) -> ", "$tmp") +
            [{"ble_mtu": [], "to": "$tmp"}, {"ble_request_mtu": [], "to": "$tmp2"}] +
            log("mtu before=", "$tmp", " request_mtu -> ", "$tmp2")},
        "ble_mtu": {"args": ["mtu"], "actions":
            log("ble_mtu event: ", "$mtu") +
            [{"ble_subscribe": "150B", "to": "$tmp"}] + log("subscribe(150B) -> ", "$tmp") +
            [{"ble_read": "1500", "to": "$tmp"}] + log("read while subscribe pending (expect -1) -> ", "$tmp")},
        "ble_subscribed": {"args": ["status"], "actions":
            log("ble_subscribed event: ", "$status") +
            [{"ble_read": "1500", "to": "$tmp"}] + log("read(1500 battery) -> ", "$tmp")},
        "ble_read": {"args": ["len"], "actions":
            [{"ble_rx_uuid": [], "to": "$tmp"}, {"ble_rx_byte": 0, "to": "$tmp2"}] +
            log("ble_read event: len=", "$len", " uuid=", "$tmp", " byte0(battery %)=", "$tmp2") +
            [{"bytes_clear": "$bf"}] + b("$bf", 0xBF, 0, 0, 128, 128, 128, 128) +   # soft limits A,B = 0
            [{"ble_write_no_response": "$bf"}, {"bytes_to_hex": "$bf", "to": "$tmp"}] +
            log("sent BF (soft limits 0) ", "$tmp") + [{"set": ["$started", 1]}]},
        "tick": {"args": ["now"], "actions": [
            {"if": {"eq": ["$started", 1], "then": [
                {"sub": ["$now", "$last"]},
                {"if": {"gte": ["$_", 100], "then": [
                    {"set": ["$last", "$now"]}, {"inc": "$sent"},
                    {"set": ["$hdr", 15]},
                    {"mod": ["$sent", 10]},
                    {"if": {"eq": ["$_", 0], "then": [{"set": ["$hdr", 31]}]}},   # seq 1: ask for B1
                ] + zero_b0 + [
                    {"ble_tx_busy": [], "to": "$tmp3"},
                    {"ble_write_no_response": "$pkt"},
                    {"if": {"eq": ["$sent", 1], "then": [{"bytes_to_hex": "$pkt", "to": "$tmp"}] +
                        log("first B0 ", "$tmp", " tx_busy before=", "$tmp3")}},
                    {"if": {"gte": ["$sent", 30], "then": [{"set": ["$started", 2]}] +
                        log("sent 30 zero B0 packets (3 s)")}},
                ]}},
            ]}},
        ]},
        "ble_notify": {"args": ["len"], "actions":
            [{"ble_rx_uuid": [], "to": "$tmp"}, {"ble_rx_copy": "$rx", "to": "$tmp2"},
             {"bytes_to_hex": "$rx", "to": "$tmp3"}] +
            log("ble_notify: len=", "$len", " uuid=", "$tmp", " data=", "$tmp3")},
        "disconnect": {"actions": log("coyote-test: disconnected")},
    },
}
open("coyote-test.min.json", "w").write(json.dumps(plugin, separators=(",", ":")))
print("coyote-test.min.json", len(json.dumps(plugin, separators=(",", ":"))))
