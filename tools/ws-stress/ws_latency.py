"""Probe command round-trip latency once per second for N seconds."""
import asyncio, json, sys, time
import websockets

async def main(host, seconds):
    worst, fails, n = 0.0, 0, 0
    end = time.time() + seconds
    while time.time() < end:
        t = time.time()
        try:
            async with websockets.connect(f"ws://{host}/", open_timeout=8, close_timeout=2) as ws:
                await ws.send(json.dumps({"info": None}))
                await asyncio.wait_for(ws.recv(), 8)
            dt = time.time() - t; worst = max(worst, dt)
        except Exception as e:
            fails += 1; print("probe fail:", type(e).__name__, flush=True)
        n += 1
        await asyncio.sleep(max(0, 1 - (time.time() - t)))
    print("latency probes: %d, failures: %d, worst round trip: %.2fs" % (n, fails, worst), flush=True)

asyncio.run(main(sys.argv[1], float(sys.argv[2])))
