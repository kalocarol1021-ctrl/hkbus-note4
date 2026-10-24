#!/usr/bin/env python3
"""Pretend to be the bus board so the settings page can be tried on a PC.

  python3 tools/mock_device.py [--setup] [--port 8080]
  then open http://localhost:8080/

Saved configuration is printed and written to /tmp/hkbus_mock_config.json.
With --setup the page behaves as on the device's own set-up Wi-Fi.
The settings code is ABC234 (open http://localhost:8080/?k=ABC234 to skip it).
"""
import argparse
import json
import os
import struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INDEX = os.path.join(ROOT, "main", "web", "index.html")
OUT = "/tmp/hkbus_mock_config.json"
CODE = "ABC234"
SETUP = False

state = {
    "version": "mock", "mode": "normal", "connected": True, "ssid": "MyHome",
    "ip": "127.0.0.1", "rssi": -52, "ap_ssid": "HKBus-1A2B", "time": 0,
    "battery": -1,
    "update": {"state": "idle", "progress": 0, "error": ""},
    "config": {"v": 1, "title": "", "rows": 4, "cycle": 0,
               "hours": {"on": False, "ranges": []}, "items": []},
    "rows": [],
}


def blank_bmp():
    w, h, row = 400, 300, 52
    header = struct.pack("<2sIHHI", b"BM", 62 + row * h, 0, 0, 62)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 1, 0, row * h, 0, 0, 2, 0)
    palette = struct.pack("<II", 0, 0xFFFFFF)
    return header + info + palette + b"\xff" * (row * h)


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self):
        if SETUP:
            return True
        query = self.path.split("?")[1] if "?" in self.path else ""
        keys = [kv.split("=", 1)[1] for kv in query.split("&") if kv.startswith("k=")]
        key = self.headers.get("X-Key") or (keys[0] if keys else "")
        return key.upper() == CODE

    def do_GET(self):
        path = self.path.split("?")[0]
        if path.startswith("/api/") or path == "/screen.bmp":
            if not self._authorized():
                return self._send(401, '{"ok":false,"error":"need_code"}')
        if path == "/":
            with open(INDEX, "rb") as f:
                self._send(200, f.read(), "text/html; charset=utf-8")
        elif path == "/api/state":
            self._send(200, json.dumps(state, ensure_ascii=False))
        elif path == "/api/scan":
            self._send(200, json.dumps([{"ssid": "MyHome", "rssi": -48, "secure": True},
                                        {"ssid": "Neighbour", "rssi": -80, "secure": True}]))
        elif path == "/api/update/check":
            self._send(200, json.dumps({"ok": True, "current": "1.1.0", "latest": "1.2.0",
                                        "available": True, "notes": "示範更新"}, ensure_ascii=False))
        elif path == "/screen.bmp":
            self._send(200, blank_bmp(), "image/bmp")
        else:
            self._send(404, "{}")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)) or 0)
        path = self.path.split("?")[0]
        if not self._authorized():
            return self._send(401, '{"ok":false,"error":"need_code"}')
        if path == "/api/config":
            cfg = json.loads(body)
            state["config"] = cfg
            state["rows"] = [{"loaded": True, "failed": False, "remark": "", "mins": [3, 12]}
                             for _ in cfg.get("items", [])]
            with open(OUT, "w", encoding="utf-8") as f:
                json.dump(cfg, f, ensure_ascii=False, indent=1)
            print("saved config ->", OUT)
            self._send(200, '{"ok":true}')
        elif path == "/api/update/install":
            state["update"] = {"state": "done", "progress": 100, "error": ""}
            self._send(200, '{"ok":true}')
        elif path in ("/api/refresh", "/api/wifi"):
            print(path, body.decode(errors="replace"))
            self._send(200, '{"ok":true}')
        else:
            self._send(404, "{}")

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--setup", action="store_true")
    a = ap.parse_args()
    if a.setup:
        SETUP = True
        state.update(mode="setup", connected=False, ip="")
    print(f"http://localhost:{a.port}/")
    ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()
