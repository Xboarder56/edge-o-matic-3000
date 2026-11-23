"""Test C: open N websocket connections at once (more than the server's slots), subscribe them to
readings, then drop them all abruptly (TCP reset, no close handshake)."""
import base64, json, os, socket, struct, sys, time

def ws_frame(text):
    payload = text.encode(); mask = os.urandom(4)
    return bytes([0x81, 0x80 | len(payload)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))

host, n = sys.argv[1], int(sys.argv[2])
socks, ok = [], 0
for i in range(n):
    try:
        s = socket.create_connection((host, 80), timeout=3)
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall(("GET / HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (host, key)).encode())
        if b"101" in s.recv(200):
            ok += 1
            s.sendall(ws_frame(json.dumps({"streamReadings": True})))
        socks.append(s)
    except Exception as e:
        print("conn %d: %s" % (i, e))
print("opened %d/%d websocket sessions" % (ok, n), flush=True)
time.sleep(5)
for s in socks:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()
print("dropped all", flush=True)
