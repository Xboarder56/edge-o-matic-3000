"""Generate the net-test feature plugin (HTTP host function checks)."""
import json, sys
if len(sys.argv) < 2:
    sys.exit("usage: gen_net_test.py <server-ip> [second-ip]  (this machine's LAN address)")
MAC = sys.argv[1]
ALT = sys.argv[2] if len(sys.argv) > 2 else MAC  # loop alternates between the two

def log(*parts):
    # concat takes strings only: convert variables with to_string first.
    acts, out = [], []
    for i, p in enumerate(parts):
        if isinstance(p, str) and p.startswith("$"):
            acts.append({"to_string": p, "to": "$s%d" % i}); out.append("$s%d" % i)
        else:
            out.append(p)
    return acts + [{"concat": out}, {"log": "$_"}]

def step(n, actions):
    return {"if": {"eq": ["$step", n], "then": actions}}

def req(label, *args):
    return [{"http_request": list(args), "to": "$rid"}] + log(label, " -> ", "$rid")

steps = [
    req("t1 GET http json", "GET", f"http://{MAC}:8765/json"),
    [{"http_set_header": ["X-Test", "eom-plugin"]}] +
        req("t2 POST json + header", "POST", f"http://{MAC}:8765/echo", '{"a":1,"b":"two"}'),
    req("t3 GET https verified (github pages, redirect)", "GET",
        "https://xboarder56.github.io/edge-o-matic-3000"),
    req("t4 GET https self-signed, verify on (expect failure)", "GET", f"https://{MAC}:8766/json"),
    req("t5 GET https self-signed, flags=1 skip verify", "GET", f"https://{MAC}:8766/json", "", 1),
    req("t6 GET closed port (expect -1)", "GET", f"http://{MAC}:9/"),
    req("t7a GET slow", "GET", f"http://{MAC}:8765/slow") +
        [{"http_busy": [], "to": "$tmp"}] + log("t7b http_busy -> ", "$tmp") +
        req("t7c second request while busy (expect -1)", "GET", f"http://{MAC}:8765/json"),
    req("t8a bad method (expect -2)", "FETCH", f"http://{MAC}:8765/json") +
        req("t8b bad scheme (expect -2)", "GET", "ftp://example.com/") +
        [{"http_response_body": [], "to": "$tmp"}] + log("t8c body outside handler: '", "$tmp", "'"),
    req("t9 GET 404", "GET", f"http://{MAC}:8765/missing"),
    req("t10 GET large body (expect truncation, length -1)", "GET", f"http://{MAC}:8765/big"),
    [{"hex_to_bytes": ["0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "$key"]},
     {"hmac_sha256": ["$key", "Hi There", "$mac"]}, {"bytes_to_hex": "$mac", "to": "$tmp"}] +
        log("t11 hmac rfc4231#1 ", "$tmp") +
        [{"sha256": ["abc", "$mac"]}, {"bytes_to_hex": "$mac", "to": "$tmp"}] + log("t11 sha256(abc) ", "$tmp") +
        [{"bytes_clear": "$buf"}, {"bytes_append": ["$buf", "OST"]}, {"bytes_append_int": ["$buf", 305419896, 4]},
         {"bytes_append_int": ["$buf", 4660, -2]}, {"bytes_to_hex": "$buf", "to": "$tmp"}] + log("t11 append ", "$tmp") +
        [{"bytes_read_int": ["$buf", 3, 4], "to": "$tmp"}] + log("t11 read_int le32 ", "$tmp") +
        [{"bytes_equal": ["$buf", "OSTx", 3], "to": "$tmp"}] + log("t11 equal3 ", "$tmp") +
        [{"random_bytes": ["$buf", 8], "to": "$tmp"}, {"bytes_to_hex": "$buf", "to": "$tmp2"}] + log("t11 random ", "$tmp", " ", "$tmp2") +
        [{"hex_to_bytes": ["zz", "$buf"], "to": "$tmp"}] + log("t11 bad hex -> ", "$tmp"),
]

plugin = {
    "name": "net-test",
    "display_name": "Net Test",
    "version": "0.1.0",
    "sdk_version": "~> 1.1.0",
    "type": "feature",
    "description": "Firmware HTTP/bytes host function checks (step per mode_set).",
    "author": "test",
    "license": "MIT",
    "platforms": ["eom3k ~> 2.1.0"],
    "permissions": ["net:http"],
    "variables": {"s0": "", "s1": "", "s2": "", "s3": "", "s4": "", "s5": "", "s6": "", "s7": "", "s8": "", "s9": "", "step": 0, "rid": 0, "tmp": 0, "tmp2": 0, "now": 0,
                  "key[32]": 0, "mac[32]": 0, "buf[32]": 0},
    "functions": {},
    "events": {
        "mode_set": {
            "args": ["mode"],
            "actions": [
                {"millis": [], "to": "$now"},
                {"if": {"lt": ["$now", 20000], "then": [{"return": 0}]}},
                {"inc": "$step"},
            ] + log("== step ", "$step") + [step(i + 1, a) for i, a in enumerate(steps)] + [
                # Reliability loop after the fixed steps: alternate the Mac's two addresses.
                {"if": {"gt": ["$step", len(steps)], "then": [
                    {"mod": ["$step", 2]},
                    {"if": {"eq": ["$_", 0], "then": req("loop A", "GET", f"http://{MAC}:8765/json"),
                            "else": req("loop B", "GET", f"http://{ALT}:8765/missing")}},
                ]}},
            ],
        },
        "http_response": {
            "args": ["status"],
            "actions": (
                [{"http_response_id": [], "to": "$tmp"}, {"http_response_length": [], "to": "$tmp2"}]
                + log("resp id=", "$tmp", " status=", "$status", " len=", "$tmp2")
                + [{"http_json_string": "state", "to": "$tmp"}, {"http_json_int": "remaining_ms", "to": "$tmp2"}]
                + log("  json state=", "$tmp", " remaining_ms=", "$tmp2")
                + [{"http_json_string": "nested.arr.2.x", "to": "$tmp"}, {"http_json_has": "nested.missing", "to": "$tmp2"}]
                + log("  json nested.arr.2.x=", "$tmp", " has(missing)=", "$tmp2")
                + [{"http_json_string": "version", "to": "$tmp"}, {"http_json_string": "assets.0.role", "to": "$tmp2"}]
                + log("  json version=", "$tmp", " assets.0.role=", "$tmp2")
                + [{"http_json_string": "ct", "to": "$tmp"}, {"http_json_string": "xtest", "to": "$tmp2"}]
                + log("  echo ct=", "$tmp", " xtest=", "$tmp2")
                + [{"http_json_string": "body", "to": "$tmp"}, {"http_json_string": "error", "to": "$tmp2"}]
                + log("  echo body=", "$tmp", " error=", "$tmp2")
            ),
        },
    },
}
json.dump(plugin, open("net-test.json", "w"), indent=1)
print("wrote net-test.json, %d bytes" % len(json.dumps(plugin)))
