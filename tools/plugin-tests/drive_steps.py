"""Trigger net-test steps: each {"setMode": "manual"} fires mode_set -> next step."""
import asyncio, json, sys, websockets
HOST = sys.argv[1]; N = int(sys.argv[2]); GAP = float(sys.argv[3]) if len(sys.argv) > 3 else 7

async def main():
    for i in range(1, N + 1):
        async with websockets.connect(f"ws://{HOST}/", open_timeout=5) as ws:
            await ws.send(json.dumps({"setMode": "manual"}))
            await asyncio.wait_for(ws.recv(), 5)
        print(f"step {i} triggered", flush=True)
        await asyncio.sleep(GAP)

asyncio.run(main())
