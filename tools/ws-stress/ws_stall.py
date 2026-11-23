"""Test B: a websocket client that subscribes to the readings stream and then stops reading
(tiny receive buffer, like a sleeping phone). Blocking server-side sends stall whichever task
does the broadcasting. Afterwards the socket is reset (no close handshake)."""
import base64, json, os, socket, struct, sys, time

def ws_frame(text):
    payload = text.encode(); mask = os.urandom(4)
    hdr = bytes([0x81, 0x80 | len(payload)]) + mask
    return hdr + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))

host, hold = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 40
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256)
s.settimeout(5)
s.connect((host, 80))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET / HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (host, key)).encode())
print("handshake:", s.recv(200).split(b"\r\n")[0].decode(), flush=True)
s.sendall(ws_frame(json.dumps({"streamReadings": True})))
print("subscribed; not reading for %.0fs" % hold, flush=True)
time.sleep(hold)
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()
print("reset connection", flush=True)
