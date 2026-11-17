#!/usr/bin/env python3
"""Mock of the board's web API, for working on the web page without hardware.
Usage: python3 test/mock_server.py [port] [--setup|--newpass]   then open http://localhost:8080/
The parameter table comes from the real firmware (pass a schema JSON made by the host build as SCHEMA env)."""
import json, math, os, sys, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(ROOT, "..", "firmware", "web", "index.html")
SCHEMA = json.load(open(os.environ.get("SCHEMA", "/tmp/schema.json")))
MODE = "setup" if "--setup" in sys.argv else "newpass" if "--newpass" in sys.argv else "app"
T0 = time.time()
INPUTS = ["3bt", "limit1", "limit2", "ped", "safety", "phantom", "exit", "reset", "radio", "edge", "mid", "spare"]
held = {k: False for k in INPUTS if k not in ("limit1", "limit2")}


def status():
    t = time.time() - T0
    pos = min(100, t * 5) if t < 20 else 100
    s = {"state": "opening" if pos < 100 else "open", "position": round(pos), "direction": "opening" if pos < 100 else "none",
         "autoclose_left": -1 if pos < 100 else 12, "hold_open": False, "auto_close": True, "lock_out": False,
         "motor": "MOTOR-1" if pos < 100 else "off", "current": 4.52 if pos < 100 else 0, "peak": 9.0, "baseline": 4.5,
         "threshold": 6.69, "open_limit": pos >= 100, "closed_limit": pos <= 0, "limit_type": "NC", "dip1": False}
    for k in INPUTS:
        s["input/" + k] = held.get(k, False)
        s["input/" + k + "/contact"] = "closed" if k in ("limit1", "safety", "edge") else "open"
    s.update({"edge_mv": -1, "button": "none", "lock": False, "relay": False, "alarm": False, "fault/lockout": False,
              "fault/obstructions_run": 0, "fault/no_current": False, "fault/motor_latched": False, "fault/max_run": False,
              "fault/both_limits": False, "fault/forced_open": False, "fault/edge_wiring": False, "cycles": 37, "obstructions": 2,
              "last_obstruction": 1763340000, "travel_open": 20.1, "travel_close": 19.8, "mid_position": 50, "firmware": "0.2.0",
              "hardware": "simulator", "uptime": int(t) + 3600, "reset_reason": "power on", "free_heap": 182000, "name": "Driveway gate",
              "ssid": "HomeNet", "rssi": -58, "ip": "192.168.1.77", "mac": "24:6F:28:A1:B2:C3", "host": "gatemaster-a1b2.local",
              "mqtt": True, "setup_network": False, "update_available": False, "latest_version": "0.2.0"})
    s["sim"] = {"pos": pos, "amps": s["current"], "travel": 20, "limits": "NC", "obstacle": 60, "obstacleBlocksOpening": False,
                "trip": False, "dip1": False, "edgeFault": False, "stalled": False, "held": held}
    return s


def graph(last):
    n = 200 if last else min(200, int((time.time() - T0) * 10))
    a = [90 if i < 5 else int(45 + 2 * math.sin(i / 3)) for i in range(n)]
    if last:
        a = [90 if i < 5 else int(44 + 2 * math.cos(i / 4)) for i in range(n)]
    t = [80 if i < 10 else 67 for i in range(n)]
    return {"run": 2 if last else 3, "from": 0, "len": n, "a": a, "t": t}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def send(self, code, body, ctype="application/json"):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b))); self.end_headers()
        self.wfile.write(b)

    def j(self, o, code=200): self.send(code, json.dumps(o))

    def do_GET(self):
        p = urllib.parse.urlparse(self.path); q = dict(urllib.parse.parse_qsl(p.query))
        if p.path == "/": return self.send(200, open(PAGE, "rb").read(), "text/html")
        if p.path == "/api/info":
            return self.j({"name": "Driveway gate", "host": "gatemaster-a1b2", "fw": "0.2.0", "passwordSet": MODE == "app",
                           "setupOnly": MODE == "setup", "sim": True, "wifiSet": True})
        if p.path == "/api/status": return self.j(status())
        if p.path == "/api/config":
            return self.j({"params": SCHEMA["params"], "schema": SCHEMA["schema"],
                           "dev": {"name": "Driveway gate", "host": "gatemaster-a1b2", "wifiSsid": "HomeNet", "mqttHost": "192.168.1.129",
                                   "mqttPort": 1883, "mqttUser": "gate", "mqttPassSet": True, "mqttBase": "", "mqttBaseEffective": "gatemaster/driveway_gate",
                                   "haDiscovery": True, "tz": "PST8PDT,M3.2.0,M11.1.0", "hwMode": 1, "hwModeRunning": 1, "arduinoOta": False},
                           "update": {"version": "0.2.0", "checked": True, "available": False, "latest": "0.2.0", "date": "2026-11-17", "notes": "",
                                      "checkedAgoS": 30, "installing": False, "progress": 0, "error": "", "pendingVerify": False}})
        if p.path == "/api/update":
            return self.j({"version": "0.2.0", "checked": True, "available": False, "latest": "0.2.0", "date": "2026-11-17", "notes": "",
                           "checkedAgoS": 30, "installing": False, "progress": 0, "error": "", "pendingVerify": False})
        if p.path == "/api/current": return self.j(graph(q.get("last") == "1"))
        if p.path == "/api/log":
            ev = [{"seq": i, "t": 1763400000 + i * 7, "up": i * 7, "time": "", "text": txt} for i, txt in enumerate(
                ["GateMaster 0.2.0 on SIMULATOR (DevKit), restart reason: power on", "start: gate closed, DIP1 OFF, limits NC",
                 "Wi-Fi connected to HomeNet, IP 192.168.1.77, http://gatemaster-a1b2.local", "MQTT connected to 192.168.1.129, topic gatemaster/driveway_gate",
                 "input exit active", "open (EXIT)", "input exit clear"], 1)]
            since = int(q.get("since", 0))
            return self.j({"latest": 7, "events": [e for e in ev if e["seq"] > since]})
        if p.path == "/api/scan": return self.j([{"ssid": "HomeNet", "rssi": -51, "open": False}, {"ssid": "Neighbour", "rssi": -80, "open": False}])
        self.send(404, "not found", "text/plain")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n).decode(errors="replace")
        f = dict(urllib.parse.parse_qsl(body))
        p = urllib.parse.urlparse(self.path).path
        if p == "/api/cmd":
            if f.get("cmd") == "close": return self.j({"ok": False, "error": "photo-eye active"}, 409)
            return self.j({"ok": True})
        if p == "/api/sim":
            name, v = f.get("name", ""), f.get("value", "")
            if name in held and v in ("on", "off"): held[name] = v == "on"
            return self.j({"ok": True, "sim": status()["sim"]})
        if p == "/api/config": return self.j({"ok": True, "error": "", "reboot": False})
        return self.j({"ok": True})


port = int(next((a for a in sys.argv[1:] if a.isdigit()), 8080))
print(f"mock GateMaster ({MODE}) on http://localhost:{port}/")
ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
