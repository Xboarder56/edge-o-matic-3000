"""Serial console session for a console-enabled build (see README). One session (opening the port resets the board): wait for READY, run steps, log.

  eomsession.py <logfile> <seconds> [put:<local>:<remote>] [cmd:<console line>] [restart] ...
After the steps, keeps appending device output to <logfile> for <seconds>.
"""
import os, serial, sys, time

PORT = os.environ.get("EOM_PORT")  # e.g. /dev/cu.usbserial-XXXX or /dev/ttyUSB0
if not PORT:
    sys.exit("Set EOM_PORT to the device's serial port")
s = serial.Serial(); s.port = PORT; s.baudrate = 115200; s.timeout = 0.1
s.dtr = False; s.rts = False; s.open()
log = open(sys.argv[1], "ab")

def pump(seconds, until=None):
    buf = b""; end = time.time() + seconds
    while time.time() < end:
        d = s.read(4096)
        if d:
            log.write(d); log.flush(); buf += d
            if until and until.encode() in buf: return True, buf
    return False, buf

ok, _ = pump(25, "READY.")
print("ready" if ok else "no READY seen", flush=True)
pump(1)

for step in sys.argv[3:]:
    if step.startswith("put:"):
        _, local, remote = step.split(":", 2)
        data = open(local, "rb").read()
        s.write(f"fput {remote}\n".encode()); s.flush()
        ok, _ = pump(5, ">>>SENDHEX")
        if not ok: print("fput: no prompt"); continue
        h = data.hex().encode()
        # No flow control and the console echoes 3 chars per 2 received: throttle hard.
        for i in range(0, len(h), 32):
            s.write(h[i:i + 32]); s.flush(); pump(0.06)
        s.write(b"\n\n\n\n"); s.flush()
        ok, buf = pump(10, "<<<RECV:")
        tail = buf.decode(errors="replace").split("<<<RECV:")[-1].splitlines()[0] if ok else "?"
        print(f"put {remote}: sent {len(data)}, device received {tail}", flush=True)
    elif step.startswith("cmd:"):
        s.write(step[4:].encode() + b"\n"); s.flush()
        _, buf = pump(2)
        print(buf.decode(errors="replace")[-1500:], flush=True)
    elif step == "restart":
        s.write(b"restart\n"); s.flush()
        ok, _ = pump(25, "READY.")
        print("restarted" if ok else "restart: no READY", flush=True)

print("logging...", flush=True)
pump(float(sys.argv[2]))
