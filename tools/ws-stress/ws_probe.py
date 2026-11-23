"""Send one websocket command to the EOM and print the reply (or the failure)."""
import asyncio, json, sys, time
import websockets

async def main(host, timeout=5):
    t = time.time()
    try:
        async with websockets.connect(f"ws://{host}/", open_timeout=timeout, close_timeout=1) as ws:
            await ws.send(json.dumps({"info": None}))
            reply = await asyncio.wait_for(ws.recv(), timeout)
            print("OK %.2fs %s" % (time.time() - t, reply[:160]))
    except Exception as e:
        print("FAIL %.2fs %s: %s" % (time.time() - t, type(e).__name__, e))

asyncio.run(main(sys.argv[1]))
