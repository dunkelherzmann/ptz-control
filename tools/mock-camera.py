#!/usr/bin/env python3
"""Mock PTZ camera for developing PTZ Control without hardware.

Speaks ONVIF (SOAP 1.2, WS-Security UsernameToken digest is verified) and
Axis VAPIX ptz.cgi (HTTP Digest is verified) on one port, and serves a tiny
web page at /. Positions glide toward each AbsoluteMove target.

    python3 tools/mock-camera.py --port 8080 --user admin --password secret
"""
import argparse
import base64
import hashlib
import os
import re
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

parser = argparse.ArgumentParser()
parser.add_argument("--port", type=int, default=8080)
parser.add_argument("--user", default="admin")
parser.add_argument("--password", default="secret")
args = parser.parse_args()

lock = threading.Lock()
onvif = {"pan": 0.0, "tilt": 0.0, "zoom": 0.0, "target": None, "since": 0.0}
axis = {"pan": 0.0, "tilt": 0.0, "zoom": 1, "target": None, "since": 0.0}
REALM = "AXIS_MOCK"


def glide(state, keys):
    """Advance toward the target over ~2 seconds; report whether still moving."""
    if not state["target"]:
        return False
    progress = min(1.0, (time.time() - state["since"]) / 2.0)
    start, target = state["start"], state["target"]
    for k in keys:
        if k in target:
            state[k] = start[k] + (target[k] - start[k]) * progress
    if progress >= 1.0:
        state["target"] = None
        return False
    return True


def soap(body):
    return ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
            'xmlns:tt="http://www.onvif.org/ver10/schema" xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
            'xmlns:trt="http://www.onvif.org/ver10/media/wsdl" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl">'
            f"<s:Body>{body}</s:Body></s:Envelope>")


def fault(reason, code="ter:NotAuthorized"):
    return soap('<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>'
                f"{code}</s:Value></s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">{reason}</s:Text>"
                "</s:Reason></s:Fault>")


PT_SPACE = "http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace"
Z_SPACE = "http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        print("mock:", fmt % a)

    def reply(self, status, body, ctype="application/soap+xml; charset=utf-8", headers=()):
        data = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    # ---- ONVIF ---------------------------------------------------------------
    def wsse_ok(self, xml):
        m = lambda tag: re.search(rf"<(?:\w+:)?{tag}[^>]*>([^<]*)</", xml)
        user, digest, nonce, created = m("Username"), m("Password"), m("Nonce"), m("Created")
        if not all((user, digest, nonce, created)) or user.group(1) != args.user:
            return False
        expected = base64.b64encode(hashlib.sha1(
            base64.b64decode(nonce.group(1)) + created.group(1).encode() + args.password.encode()).digest()).decode()
        return expected == digest.group(1)

    def do_POST(self):
        xml = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        op = re.search(r"<s:Body><(?:\w+:)?(\w+)", xml)
        op = op.group(1) if op else ""
        base = f"http://{self.headers.get('Host', '127.0.0.1')}"
        if op == "GetSystemDateAndTime":
            now = datetime.now(timezone.utc)
            return self.reply(200, soap(
                "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:UTCDateTime>"
                f"<tt:Time><tt:Hour>{now.hour}</tt:Hour><tt:Minute>{now.minute}</tt:Minute><tt:Second>{now.second}</tt:Second></tt:Time>"
                f"<tt:Date><tt:Year>{now.year}</tt:Year><tt:Month>{now.month}</tt:Month><tt:Day>{now.day}</tt:Day></tt:Date>"
                "</tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>"))
        if not self.wsse_ok(xml):
            return self.reply(400, fault("Sender not authorized"))
        print("mock: onvif", op)
        if op == "GetCapabilities":
            return self.reply(200, soap(
                "<tds:GetCapabilitiesResponse><tds:Capabilities>"
                f"<tt:Media><tt:XAddr>http://10.99.0.5/onvif/media</tt:XAddr></tt:Media>"
                f"<tt:PTZ><tt:XAddr>http://10.99.0.5/onvif/ptz</tt:XAddr></tt:PTZ>"
                "</tds:Capabilities></tds:GetCapabilitiesResponse>".replace("http://10.99.0.5", base)))
        if op == "GetProfiles":
            return self.reply(200, soap(
                '<trt:GetProfilesResponse><trt:Profiles token="Profile_1" fixed="true"><tt:Name>main</tt:Name>'
                '<tt:PTZConfiguration token="PTZ_1"><tt:Name>ptz</tt:Name>'
                f"<tt:DefaultAbsolutePantTiltPositionSpace>{PT_SPACE}</tt:DefaultAbsolutePantTiltPositionSpace>"
                f"<tt:DefaultAbsoluteZoomPositionSpace>{Z_SPACE}</tt:DefaultAbsoluteZoomPositionSpace>"
                "</tt:PTZConfiguration></trt:Profiles></trt:GetProfilesResponse>"))
        if op == "GetConfigurationOptions":
            return self.reply(200, soap(
                "<tptz:GetConfigurationOptionsResponse><tptz:PTZConfigurationOptions><tt:Spaces>"
                f"<tt:AbsolutePanTiltPositionSpace><tt:URI>{PT_SPACE}</tt:URI>"
                "<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
                "<tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:AbsolutePanTiltPositionSpace>"
                f"<tt:AbsoluteZoomPositionSpace><tt:URI>{Z_SPACE}</tt:URI>"
                "<tt:XRange><tt:Min>0</tt:Min><tt:Max>1</tt:Max></tt:XRange></tt:AbsoluteZoomPositionSpace>"
                "</tt:Spaces></tptz:PTZConfigurationOptions></tptz:GetConfigurationOptionsResponse>"))
        if op == "GetStatus":
            with lock:
                moving = glide(onvif, ("pan", "tilt", "zoom"))
                s = "MOVING" if moving else "IDLE"
                return self.reply(200, soap(
                    "<tptz:GetStatusResponse><tptz:PTZStatus><tt:Position>"
                    f'<tt:PanTilt x="{onvif["pan"]:.6f}" y="{onvif["tilt"]:.6f}" space="{PT_SPACE}"/>'
                    f'<tt:Zoom x="{onvif["zoom"]:.6f}" space="{Z_SPACE}"/></tt:Position>'
                    f"<tt:MoveStatus><tt:PanTilt>{s}</tt:PanTilt><tt:Zoom>{s}</tt:Zoom></tt:MoveStatus>"
                    "</tptz:PTZStatus></tptz:GetStatusResponse>"))
        if op == "AbsoluteMove":
            target = {}
            pt = re.search(r'<(?:\w+:)?PanTilt x="([^"]+)" y="([^"]+)"', xml)
            z = re.search(r'<(?:\w+:)?Zoom x="([^"]+)"', xml)
            if pt:
                target.update(pan=float(pt.group(1)), tilt=float(pt.group(2)))
            if z:
                target["zoom"] = float(z.group(1))
            with lock:
                glide(onvif, ("pan", "tilt", "zoom"))
                onvif.update(start=dict(onvif), target=target, since=time.time())
            return self.reply(200, soap("<tptz:AbsoluteMoveResponse/>"))
        if op == "Stop":
            with lock:
                glide(onvif, ("pan", "tilt", "zoom"))
                onvif["target"] = None
            return self.reply(200, soap("<tptz:StopResponse/>"))
        return self.reply(400, fault(f"Unsupported operation {op}", "ter:ActionNotSupported"))

    # ---- Axis + web page -----------------------------------------------------
    def digest_ok(self):
        header = self.headers.get("Authorization", "")
        if not header.startswith("Digest "):
            return False
        f = dict(re.findall(r'(\w+)="?([^",]*)"?', header[7:]))
        ha1 = hashlib.md5(f"{args.user}:{REALM}:{args.password}".encode()).hexdigest()
        ha2 = hashlib.md5(f"GET:{f.get('uri', '')}".encode()).hexdigest()
        expected = hashlib.md5(
            f"{ha1}:{f.get('nonce')}:{f.get('nc')}:{f.get('cnonce')}:{f.get('qop')}:{ha2}".encode()).hexdigest()
        return f.get("username") == args.user and f.get("response") == expected

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == "/":
            return self.reply(200, "<html><body><h1>Mock PTZ camera</h1></body></html>", "text/html")
        if url.path != "/axis-cgi/com/ptz.cgi":
            return self.reply(404, "not found", "text/plain")
        if not self.digest_ok():
            nonce = base64.b64encode(os.urandom(12)).decode()
            return self.reply(401, "unauthorized", "text/plain", [
                ("WWW-Authenticate", f'Digest realm="{REALM}", nonce="{nonce}", qop="auth", algorithm=MD5')])
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        with lock:
            glide(axis, ("pan", "tilt", "zoom"))
            if q.get("query") == "limits":
                body = "MinPan=-180\nMaxPan=180\nMinTilt=-90\nMaxTilt=10\nMinZoom=1\nMaxZoom=9999\n"
            elif q.get("query") == "position":
                body = f"pan={axis['pan']:.4f}\ntilt={axis['tilt']:.4f}\nzoom={round(axis['zoom'])}\n"
            elif q.get("move") == "stop":
                axis["target"], body = None, ""
            else:
                target = {k: float(q[k]) for k in ("pan", "tilt", "zoom") if k in q}
                if not target:
                    body = "Error: no parameters"
                else:
                    axis.update(start=dict(axis), target=target, since=time.time())
                    body = ""
        return self.reply(200, body, "text/plain")


print(f"Mock camera on http://127.0.0.1:{args.port}  user={args.user}")
ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
