"""2.1.0-rc.5 API checks: getState, resetDenials, setLock + lock_changed, pluginList,
pluginEvent validation, getDevices, config_changed, nonce on the new commands.
Does not change the motor. Locks the device buttons for about a second, then unlocks.
Usage: python3 ws_rc5.py <device-ip>"""
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


async def command(ws, name, arg, wait=3):
    """Send one command and return its response object (other broadcasts are skipped)."""
    await ws.send(json.dumps({name: arg}))
    end = time.time() + wait
    while (left := end - time.time()) > 0:
        try:
            msg = json.loads(await asyncio.wait_for(ws.recv(), left))
        except asyncio.TimeoutError:
            break
        if name in msg:
            return msg[name]
    return None


def events(msgs, name):
    return [m["event"] for m in msgs if "event" in m and m["event"].get("name") == name]


async def state_and_denials():
    async with websockets.connect(URL) as ws:
        st = await command(ws, "getState", {"nonce": 41})
        keys = ["runMode", "mode", "motor", "arousal", "threshold", "denials", "lock",
                "detection", "pluginControl", "freeHeap", "fwVersion", "millis"]
        check("getState returns a full snapshot", st is not None and all(k in st for k in keys),
              "missing: %s" % ([k for k in keys if st is None or k not in st]))
        check("nonce echoed on getState", st is not None and st.get("nonce") == 41)
        if st:
            print("      mode=%s motor=%s arousal=%s denials=%s lock=%s freeHeap=%s fw=%s" % (
                st.get("runMode"), st.get("motor"), st.get("arousal"), st.get("denials"),
                st.get("lock"), st.get("freeHeap"), st.get("fwVersion")))

        r = await command(ws, "resetDenials", {})
        check("resetDenials sets the counter to 0", r is not None and r.get("denials") == 0, r)


async def lock():
    async with websockets.connect(URL) as ctl, websockets.connect(URL) as sub:
        await sub.send(json.dumps({"streamEvents": True}))
        await asyncio.sleep(0.3)

        r = await command(ctl, "setLock", {"locked": True, "reason": "ws_rc5 test"})
        check("setLock true locks", r is not None and r.get("locked") is True, r)
        st = await command(ctl, "getState", {})
        check("getState shows the lock", st is not None and st["lock"]["locked"] is True
              and st["lock"]["reason"] == "ws_rc5 test")

        r = await command(ctl, "setLock", {"locked": False})
        check("setLock false unlocks", r is not None and r.get("locked") is False, r)

        msgs = await collect(sub, 1.5)
        ev = events(msgs, "lock_changed")
        check("lock_changed events: locked then unlocked",
              len(ev) >= 2 and ev[0].get("locked") is True and ev[0].get("reason") == "ws_rc5 test"
              and ev[-1].get("locked") is False, ev)

        r = await command(ctl, "setLock", {"reason": "no locked flag"})
        check("setLock without 'locked' is rejected", r is not None and r.get("errno") == 1, r)


async def plugins_and_devices():
    async with websockets.connect(URL) as ws:
        r = await command(ws, "pluginList", {})
        plugins = (r or {}).get("plugins")
        check("pluginList returns an array", isinstance(plugins, list),
              [(p["name"], p["type"]) for p in plugins or []])

        r = await command(ws, "pluginEvent", {"plugin": "no-such-plugin", "event": "x"})
        check("pluginEvent: unknown plugin is an error",
              r is not None and r.get("errno") == 1 and "not loaded" in r.get("error", ""), r)

        r = await command(ws, "pluginEvent", {"plugin": "x", "event": "Bad Name!"})
        check("pluginEvent: invalid event name rejected", r is not None and r.get("errno") == 1, r)

        feature = [p for p in plugins or [] if p["type"] != "ble_driver"]
        if feature:
            name = feature[0]["name"]
            r = await command(ws, "pluginEvent", {"plugin": name, "event": "ws_rc5_missing"})
            check("pluginEvent: missing remote_ handler reported",
                  r is not None and r.get("event") == "remote_ws_rc5_missing"
                  and "no handler" in r.get("error", ""), r)

        driver = [p for p in plugins or [] if p["type"] == "ble_driver"]
        if driver:
            r = await command(ws, "pluginEvent", {"plugin": driver[0]["name"], "event": "x"})
            check("pluginEvent: driver plugins refused",
                  r is not None and "driver" in r.get("error", ""), r)

        r = await command(ws, "getDevices", {})
        check("getDevices returns an array", r is not None and isinstance(r.get("devices"), list),
              r)


async def config_changed():
    async with websockets.connect(URL) as ctl, websockets.connect(URL) as sub:
        await sub.send(json.dumps({"streamEvents": True}))
        cfg = await command(ctl, "configList", {})
        if cfg is None or "screen_dim_seconds" not in cfg:
            check("config_changed (configList readable)", False)
            return
        # Re-save the current value: changes nothing, but triggers a save.
        await ctl.send(json.dumps({"configSet": {"screen_dim_seconds": cfg["screen_dim_seconds"]}}))
        msgs = await collect(sub, 3)
        check("config_changed fires after a save", len(events(msgs, "config_changed")) >= 1)


async def main():
    for test in (state_and_denials, lock, plugins_and_devices, config_changed):
        try:
            await test()
        except Exception as e:  # keep going so one failure does not hide the rest
            check(test.__name__, False, repr(e))
    print("\n%d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


asyncio.run(main())
