"""Test servers for the net-test plugin: HTTP on 8765, self-signed HTTPS on 8766."""
import http.server, json, ssl, sys, threading, time

DOC = {"state": "running", "remaining_ms": 897412, "nested": {"arr": [1, 2, {"x": "y"}]}}

class H(http.server.BaseHTTPRequestHandler):
    def _send(self, code, obj=None, raw=None):
        body = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        print("GET", self.path, dict(self.headers), flush=True)
        if self.path.startswith("/json"): self._send(200, DOC)
        elif self.path.startswith("/slow"): time.sleep(3); self._send(200, {"state": "slow"})
        elif self.path.startswith("/big"): self._send(200, raw=b'{"state":"big","pad":"' + b"x" * 6000 + b'"}')
        else: self._send(404, {"error": "missing"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n).decode()
        print("POST", self.path, dict(self.headers), body, flush=True)
        self._send(200, {"got": n, "ct": self.headers.get("Content-Type"),
                         "xtest": self.headers.get("X-Test"), "body": body})

    def log_message(self, *a): pass

def serve(port, tls):
    s = http.server.ThreadingHTTPServer(("0.0.0.0", port), H)
    if tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain("cert.pem", "key.pem")
        s.socket = ctx.wrap_socket(s.socket, server_side=True)
    s.serve_forever()

threading.Thread(target=serve, args=(8766, True), daemon=True).start()
print("servers up", flush=True)
serve(8765, False)
