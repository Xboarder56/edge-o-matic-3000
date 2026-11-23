"""Test A: repeated clean websocket connect -> command -> close. A socket leak shows up as
connections failing after a few cycles."""
import asyncio, json, sys, time
import websockets

async def one(host, i):
    t = time.time()
    try:
        async with websockets.connect(f"ws://{host}/", open_timeout=5, close_timeout=2) as ws:
            await ws.send(json.dumps({"info": None}))
            await asyncio.wait_for(ws.recv(), 5)
        return "ok", time.time() - t
    except Exception as e:
        return type(e).__name__, time.time() - t

async def main(host, n):
    for i in range(n):
        r, dt = await one(host, i)
        print("cycle %2d: %-22s %.2fs" % (i + 1, r, dt), flush=True)
        await asyncio.sleep(0.5)

asyncio.run(main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 10))
