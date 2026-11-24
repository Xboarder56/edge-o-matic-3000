"""2.1.0-rc.3 API checks: RFC 6455 ping/pong and close, streamReadings false, streamEvents,
denial count in readings, setMode argument validation. Usage: python3 ws_rc3.py <device-ip>"""
import asyncio, json, sys, time
import websockets

HOST = sys.argv[1]
URL = f"ws://{HOST}/"
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print("%s  %s  %s" % ("PASS" if ok else "FAIL", name, detail))


async def collect(ws, seconds):
    msgs, end = [], time.time() + seconds
    while (left := end - time.time()) > 0:
        try:
            msgs.append(json.loads(await asyncio.wait_for(ws.recv(), left)))
        except asyncio.TimeoutError:
            break
    return msgs


def has(msgs, key):
    return [m for m in msgs if key in m]


async def ping_pong():
    async with websockets.connect(URL, ping_interval=None, close_timeout=3) as ws:
        t = time.time()
        await asyncio.wait_for(await ws.ping(b"rc3"), 3)
        check("ping answered with pong", True, "%.0f ms" % ((time.time() - t) * 1000))
        t = time.time()
        await ws.close()
        dt = time.time() - t
        check("close handshake completes", dt < 1.0, "%.0f ms" % (dt * 1000))

    # Library keepalive pings every 1 s for 6 s; the connection must stay up.
    async with websockets.connect(URL, ping_interval=1, ping_timeout=2) as ws:
        await asyncio.sleep(6)
        await ws.send(json.dumps({"info": None}))
        await asyncio.wait_for(ws.recv(), 3)
        check("connection survives client keepalive pings", True)


async def stream_readings():
    async with websockets.connect(URL) as ws:
        await ws.send(json.dumps({"streamReadings": True}))
        msgs = await collect(ws, 3)
        readings = has(msgs, "readings")
        check("streamReadings true streams readings", len(readings) > 3, "%d in 3 s" % len(readings))
        if readings:
            r = readings[-1]["readings"]
            check("readings include denials", "denials" in r, "denials=%s" % r.get("denials"))
        await ws.send(json.dumps({"streamReadings": False}))
        await collect(ws, 0.5)
        msgs = await collect(ws, 3)
        check("streamReadings false stops readings", not has(msgs, "readings"),
              "%d readings after stop" % len(has(msgs, "readings")))


async def stream_events():
    async with websockets.connect(URL) as sub, websockets.connect(URL) as other:
        await sub.send(json.dumps({"streamEvents": True}))
        await collect(sub, 0.5)
        await other.send(json.dumps({"setMode": "manual"}))
        await other.send(json.dumps({"setMode": "automatic"}))
        await other.send(json.dumps({"setMode": "manual"}))
        ev = [m["event"] for m in has(await collect(sub, 2), "event")]
        names = [e["name"] for e in ev]
        check("streamEvents delivers mode_set", names.count("mode_set") >= 3,
              json.dumps(ev[:3]))
        leaked = has(await collect(other, 0.5), "event")
        check("unsubscribed client gets no events", not leaked)
        await sub.send(json.dumps({"streamEvents": False}))
        await collect(sub, 0.5)
        await other.send(json.dumps({"setMode": "automatic"}))
        await other.send(json.dumps({"setMode": "manual"}))
        check("streamEvents false stops events", not has(await collect(sub, 1.5), "event"))


async def set_mode_validation():
    async with websockets.connect(URL) as ws:
        for bad in (5, None, {"x": 1}):
            await ws.send(json.dumps({"setMode": bad}))
        replies = await collect(ws, 1.5)
        await ws.send(json.dumps({"info": None}))
        alive = await collect(ws, 2)
        check("setMode with non-string rejected, device alive", bool(alive),
              json.dumps(replies[:1])[:120])


async def main():
    for t in (ping_pong, stream_readings, stream_events, set_mode_validation):
        try:
            await t()
        except Exception as e:
            check(t.__name__, False, "%s: %s" % (type(e).__name__, e))
    print("\n%d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


asyncio.run(main())
