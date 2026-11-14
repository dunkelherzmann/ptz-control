#!/usr/bin/env python3
"""Mock PTZ camera for developing PTZ Control without hardware.

Speaks ONVIF (SOAP 1.2, WS-Security UsernameToken digest is verified) and
Axis VAPIX ptz.cgi (HTTP Digest is verified) on one port, and serves a tiny
web page at /. Positions glide toward each AbsoluteMove target. Pictures of a
drawn scene (PNG, standard library only) follow the camera's position and zoom,
and moves within the view (ONVIF's field-of-view space, Axis center=) aim at a
point of the picture. Live video comes over RTSP (TCP interleaved, Digest sign-in)
on --rtsp-port, looping a short H.264 or H.265 clip from tests/fixtures.

    python3 tools/mock-camera.py --port 8080 --user admin --password secret
"""
import argparse
import base64
import hashlib
import os
import re
import socket
import socketserver
import struct
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
import zlib

parser = argparse.ArgumentParser()
parser.add_argument("--port", type=int, default=8080)
parser.add_argument("--user", default="admin")
parser.add_argument("--password", default="secret")
parser.add_argument("--rtsp-port", type=int, default=0, help="default: --port + 1")
parser.add_argument("--video", choices=("h264", "h265", "none"), default="h264",
                    help="none: the stream address leads nowhere, as on a camera whose video can't be reached")
args = parser.parse_args()
args.rtsp_port = args.rtsp_port or args.port + 1

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
FOV_SPACE = "http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationSpaceFov"
PICTURE_W, PICTURE_H = 480, 270


def onvif_view():
    """Half the view's width and height in ONVIF units at the current zoom."""
    half = 0.25 * (1 - 0.8 * onvif["zoom"])
    return half, half * PICTURE_H / PICTURE_W


def axis_view():
    """Half the view's width and height in degrees at the current zoom."""
    half = 30 * (1 - 0.9 * (axis["zoom"] - 1) / 9998)
    return half, half * PICTURE_H / PICTURE_W


def aim_in_view(state, x, y, half_w, half_h, pan_range, tilt_range):
    """Glide so the point (x, y) of the picture (-1..1, up positive) comes to its centre."""
    glide(state, ("pan", "tilt", "zoom"))
    pan = min(max(state["pan"] + x * half_w, pan_range[0]), pan_range[1])
    tilt = min(max(state["tilt"] + y * half_h, tilt_range[0]), tilt_range[1])
    state.update(start=dict(state), target={"pan": pan, "tilt": tilt}, since=time.time())


def picture(pan, tilt, half_w, half_h, scale):
    """A PNG of the drawn scene around (pan, tilt); scale turns camera units into scene units."""
    rows = bytearray()
    spacing = 0.08  # landmarks: a lattice of coloured discs, each colour telling where it is
    for py in range(PICTURE_H):
        rows.append(0)
        v = (tilt + (1 - 2 * (py + 0.5) / PICTURE_H) * half_h) * scale
        if v > 0:
            base = (60, 110 + int(min(v, 0.6) * 160), 190)
        else:
            base = (55, 95 + int(max(v, -0.6) * 60), 60)
        row_v = round(v / spacing)
        dv = v - row_v * spacing
        for px in range(PICTURE_W):
            u = (pan + (2 * (px + 0.5) / PICTURE_W - 1) * half_w) * scale
            col_u = round(u / spacing)
            du = u - col_u * spacing
            d2 = du * du + dv * dv
            if d2 < (spacing * 0.28) ** 2:
                c = ((col_u * 47) % 160 + 90, (row_v * 71) % 160 + 90, ((col_u + row_v) * 37) % 120 + 60)
                if d2 > (spacing * 0.22) ** 2:
                    c = (20, 20, 24)
            elif abs(v) < 0.004:
                c = (240, 240, 240)
            else:
                c = base
            rows += bytes(c)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", PICTURE_W, PICTURE_H, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
Z_SPACE = "http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        print("mock:", fmt % a)

    def reply(self, status, body, ctype="application/soap+xml; charset=utf-8", headers=()):
        self.reply_bytes(status, body.encode(), ctype, headers)

    def reply_bytes(self, status, data, ctype, headers=()):
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
                f"<tt:RelativePanTiltTranslationSpace><tt:URI>{FOV_SPACE}</tt:URI>"
                "<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
                "<tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:RelativePanTiltTranslationSpace>"
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
        if op == "GetSnapshotUri":
            return self.reply(200, soap(
                f"<trt:GetSnapshotUriResponse><trt:MediaUri><tt:Uri>{base}/onvif/snapshot</tt:Uri>"
                "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
                "<tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetSnapshotUriResponse>"))
        if op == "GetStreamUri":
            return self.reply(200, soap(
                f"<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://127.0.0.1:{args.rtsp_port}/onvif/live"
                "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout></trt:MediaUri>"
                "</trt:GetStreamUriResponse>"))
        if op == "RelativeMove":
            pt = re.search(r'<(?:\w+:)?PanTilt x="([^"]+)" y="([^"]+)" space="([^"]+)"', xml)
            if not pt or pt.group(3) != FOV_SPACE:
                return self.reply(400, fault("Unsupported translation space", "ter:InvalidArgVal"))
            with lock:
                half_w, half_h = onvif_view()
                aim_in_view(onvif, float(pt.group(1)), float(pt.group(2)), half_w, half_h, (-1, 1), (-1, 1))
                print(f"mock: onvif view move to pan={onvif['target']['pan']:.4f} tilt={onvif['target']['tilt']:.4f}")
            return self.reply(200, soap("<tptz:RelativeMoveResponse/>"))
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
        if url.path not in ("/axis-cgi/com/ptz.cgi", "/axis-cgi/jpg/image.cgi", "/onvif/snapshot",
                            "/axis-cgi/param.cgi"):
            return self.reply(404, "not found", "text/plain")
        if not self.digest_ok():
            nonce = base64.b64encode(os.urandom(12)).decode()
            return self.reply(401, "unauthorized", "text/plain", [
                ("WWW-Authenticate", f'Digest realm="{REALM}", nonce="{nonce}", qop="auth", algorithm=MD5')])
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        if url.path == "/axis-cgi/param.cgi":
            return self.reply(200, f"root.Network.RTSP.Port={args.rtsp_port}\n", "text/plain")
        if url.path != "/axis-cgi/com/ptz.cgi":
            with lock:
                if url.path == "/onvif/snapshot":
                    glide(onvif, ("pan", "tilt", "zoom"))
                    half_w, half_h = onvif_view()
                    args_ = (onvif["pan"], onvif["tilt"], half_w, half_h, 1.0)
                else:
                    glide(axis, ("pan", "tilt", "zoom"))
                    half_w, half_h = axis_view()
                    args_ = (axis["pan"], axis["tilt"], half_w, half_h, 1 / 180)
            print("mock: picture", url.path)
            return self.reply_bytes(200, picture(*args_), "image/png")
        with lock:
            glide(axis, ("pan", "tilt", "zoom"))
            if q.get("query") == "limits":
                body = "MinPan=-180\nMaxPan=180\nMinTilt=-90\nMaxTilt=10\nMinZoom=1\nMaxZoom=9999\n"
            elif q.get("query") == "position":
                body = f"pan={axis['pan']:.4f}\ntilt={axis['tilt']:.4f}\nzoom={round(axis['zoom'])}\n"
            elif "center" in q:
                x, y = (float(v) for v in q["center"].split(","))
                w, h = float(q.get("imagewidth", PICTURE_W)), float(q.get("imageheight", PICTURE_H))
                half_w, half_h = axis_view()
                aim_in_view(axis, 2 * x / w - 1, 1 - 2 * y / h, half_w, half_h, (-180, 180), (-90, 10))
                print(f"mock: axis view move to pan={axis['target']['pan']:.4f} tilt={axis['target']['tilt']:.4f}")
                body = ""
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


# ---- RTSP live video -----------------------------------------------------------
FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures")
H265 = args.video == "h265"


def load_clip():
    """Splits the clip into frames: lists of NAL units, parameter sets kept apart for the SDP."""
    with open(os.path.join(FIXTURES, "live-h265.hevc" if H265 else "live-h264.h264"), "rb") as f:
        data = f.read()
    nals = [n.lstrip(b"\x00") for n in re.split(b"\x00\x00\x01", data)]
    nals = [n for n in nals if n]
    kind = (lambda n: (n[0] >> 1) & 0x3F) if H265 else (lambda n: n[0] & 0x1F)
    params, frames, pending = {}, [], []
    for nal in nals:
        t = kind(nal)
        if (32 <= t <= 34) if H265 else t in (7, 8):
            params.setdefault(t, nal)
        elif (t < 32) if H265 else (1 <= t <= 5):
            frames.append((pending + [nal], (16 <= t <= 21) if H265 else t == 5))
            pending = []
        else:
            pending.append(nal)
    return params, frames


CLIP_PARAMS, CLIP_FRAMES = load_clip() if args.video != "none" else ({}, [])


def sdp():
    b64 = lambda n: base64.b64encode(n).decode()
    if H265:
        p = CLIP_PARAMS
        fmtp = f"sprop-vps={b64(p[32])}; sprop-sps={b64(p[33])}; sprop-pps={b64(p[34])}"
        rtpmap = "H265/90000"
    else:
        fmtp = "packetization-mode=1; sprop-parameter-sets=" + ",".join(b64(CLIP_PARAMS[t]) for t in (7, 8))
        rtpmap = "H264/90000"
    return (f"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Mock camera\r\nt=0 0\r\na=control:*\r\n"
            f"m=audio 0 RTP/AVP 0\r\na=control:trackID=2\r\n"
            f"m=video 0 RTP/AVP 96\r\na=rtpmap:96 {rtpmap}\r\na=fmtp:96 {fmtp}\r\na=control:trackID=1\r\n")


def packets(nals):
    """RTP payloads for one frame: parameter sets aggregated, big NAL units fragmented."""
    out = []
    for nal in nals:
        if len(nal) <= 1400:
            out.append(nal)
            continue
        if H265:
            t = (nal[0] >> 1) & 0x3F
            header, fu, body = bytes([(nal[0] & 0x81) | (49 << 1), nal[1]]), t, nal[2:]
        else:
            header, fu, body = bytes([(nal[0] & 0xE0) | 28]), nal[0] & 0x1F, nal[1:]
        chunks = [body[i:i + 1400] for i in range(0, len(body), 1400)]
        for i, chunk in enumerate(chunks):
            flags = (0x80 if i == 0 else 0) | (0x40 if i == len(chunks) - 1 else 0)
            out.append(header + bytes([flags | fu]) + chunk)
    return out


def aggregate(nals):
    body = b"".join(struct.pack(">H", len(n)) + n for n in nals)
    return (bytes([48 << 1, 1]) if H265 else bytes([24])) + body


class RtspHandler(socketserver.BaseRequestHandler):
    def setup(self):
        self.nonce = base64.b64encode(os.urandom(12)).decode()
        self.send_lock = threading.Lock()
        self.playing = threading.Event()
        self.closed = threading.Event()
        self.channel = 0

    def send(self, data):
        with self.send_lock:
            self.request.sendall(data)

    def authorized(self, method, headers):
        header = headers.get("authorization", "")
        if not header.startswith("Digest "):
            return False
        f = dict(re.findall(r'(\w+)="?([^",]*)"?', header[7:]))
        ha1 = hashlib.md5(f"{args.user}:{REALM}:{args.password}".encode()).hexdigest()
        ha2 = hashlib.md5(f"{method}:{f.get('uri', '')}".encode()).hexdigest()
        if f.get("qop"):
            expected = hashlib.md5(
                f"{ha1}:{f.get('nonce')}:{f.get('nc')}:{f.get('cnonce')}:{f.get('qop')}:{ha2}".encode()).hexdigest()
        else:
            expected = hashlib.md5(f"{ha1}:{f.get('nonce')}:{ha2}".encode()).hexdigest()
        return f.get("username") == args.user and f.get("nonce") == self.nonce and f.get("response") == expected

    def reply(self, cseq, status="200 OK", headers=(), body=b""):
        text = f"RTSP/1.0 {status}\r\nCSeq: {cseq}\r\n"
        for name, value in headers:
            text += f"{name}: {value}\r\n"
        if body:
            text += f"Content-Length: {len(body)}\r\n"
        self.send(text.encode() + b"\r\n" + body)

    def stream(self):
        sequence, timestamp, sent = 1000, 12345, 0
        try:
            while not self.closed.is_set():
                for nals, key in CLIP_FRAMES:
                    if self.closed.is_set():
                        break
                    payloads = packets(nals)
                    if key:  # parameter sets in-band too, as many cameras send them
                        payloads.insert(0, aggregate([CLIP_PARAMS[t] for t in sorted(CLIP_PARAMS)]))
                    for i, payload in enumerate(payloads):
                        marker = 0x80 if i == len(payloads) - 1 else 0
                        rtp = struct.pack(">BBHII", 0x80, marker | 96, sequence & 0xFFFF, timestamp, 0x1234) + payload
                        self.send(struct.pack(">BBH", ord("$"), self.channel, len(rtp)) + rtp)
                        sequence += 1
                    sent += 1
                    timestamp = (timestamp + 9000) & 0xFFFFFFFF
                    time.sleep(0.1)
        except OSError:
            pass
        print(f"mock: rtsp sent {sent} frames")

    def handle(self):
        data = b""
        while True:
            try:
                chunk = self.request.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            data += chunk
            while b"\r\n\r\n" in data:
                head, data = data.split(b"\r\n\r\n", 1)
                lines = head.decode(errors="replace").split("\r\n")
                method, uri = lines[0].split(" ")[:2]
                headers = {}
                for line in lines[1:]:
                    if ":" in line:
                        k, v = line.split(":", 1)
                        headers[k.strip().lower()] = v.strip()
                cseq = headers.get("cseq", "0")
                if method != "OPTIONS" and not self.authorized(method, headers):
                    self.reply(cseq, "401 Unauthorized", [("WWW-Authenticate", f'Basic realm="{REALM}"'),
                               ("WWW-Authenticate", f'Digest realm="{REALM}", nonce="{self.nonce}"')])
                    continue
                if method == "OPTIONS":
                    self.reply(cseq, headers=[("Public", "OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER, TEARDOWN")])
                elif method == "DESCRIBE":
                    path = urlsplit(uri).path
                    if path not in ("/onvif/live", "/axis-media/media.amp"):
                        self.reply(cseq, "404 Not Found")
                        continue
                    print(f"mock: rtsp describe {path}")
                    self.reply(cseq, headers=[("Content-Type", "application/sdp"), ("Content-Base", uri + "/")],
                               body=sdp().encode())
                elif method == "SETUP":
                    transport = headers.get("transport", "")
                    if "TCP" not in transport.upper():
                        self.reply(cseq, "461 Unsupported Transport")
                        continue
                    m = re.search(r"interleaved=(\d+)", transport)
                    self.channel = int(m.group(1)) if m else 0
                    self.reply(cseq, headers=[("Session", "MOCK42;timeout=60"), ("Transport",
                               f"RTP/AVP/TCP;unicast;interleaved={self.channel}-{self.channel + 1}")])
                elif method == "PLAY":
                    print("mock: rtsp play")
                    self.reply(cseq, headers=[("Session", "MOCK42")])
                    if not self.playing.is_set():
                        self.playing.set()
                        threading.Thread(target=self.stream, daemon=True).start()
                elif method == "GET_PARAMETER":
                    self.reply(cseq, headers=[("Session", "MOCK42")])
                elif method == "TEARDOWN":
                    print("mock: rtsp teardown")
                    self.reply(cseq)
                    self.closed.set()
                else:
                    self.reply(cseq, "405 Method Not Allowed")
        self.closed.set()


socketserver.ThreadingTCPServer.allow_reuse_address = True
socketserver.ThreadingTCPServer.daemon_threads = True
if args.video != "none":
    rtsp_server = socketserver.ThreadingTCPServer(("127.0.0.1", args.rtsp_port), RtspHandler)
    threading.Thread(target=rtsp_server.serve_forever, daemon=True).start()

print(f"Mock camera on http://127.0.0.1:{args.port} and rtsp://127.0.0.1:{args.rtsp_port}  user={args.user}")
ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
