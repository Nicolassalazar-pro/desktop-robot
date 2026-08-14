#!/usr/bin/env python3
"""Host-side simulator for the robot override web app."""

from __future__ import annotations

import argparse
import base64
import hashlib
import http
import http.server
import json
import math
import os
import secrets
import socket
import socketserver
import struct
import sys
import threading
import time
import urllib.parse
from collections import deque
from pathlib import Path


SSID = "Hanger OVERRIDE"
PASSWORD = "TheGarageAtl"
DEFAULT_TOKEN = "deadbeefcafe1234"
OP_CONTROL = 0x01
OP_TELEMETRY = 0x12
OP_TRIM = 0x03
OP_MOOD = 0x04
OP_GESTURE = 0x05
OP_SOUND = 0x06
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
CLOSE_POLICY_VIOLATION = 1008


class ProtocolError(Exception):
    pass


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def load_override_html() -> bytes:
    src = repo_root() / "src" / "override_html.cpp"
    text = src.read_text(encoding="utf-8")
    start = text.find('R"OD(')
    end = text.rfind(')OD";')
    if start < 0 or end < 0 or end <= start:
        raise RuntimeError(
            f"{src} does not contain the expected R\"OD( ... )OD\" raw-string delimiters"
        )
    return text[start + len('R"OD(') : end].encode("utf-8")


def detect_lan_ip() -> tuple[str, str | None]:
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            # No packet is sent; connect() only asks the OS which local
            # interface it would route from, which is what phones need.
            probe.connect(("8.8.8.8", 80))
            ip = probe.getsockname()[0]
            if ip and not ip.startswith("127."):
                return ip, None
        finally:
            probe.close()
    except OSError:
        pass

    try:
        ip = socket.gethostbyname(socket.gethostname())
        return ip, "LAN IP detection fell back to socket.gethostbyname(socket.gethostname())."
    except OSError:
        return "127.0.0.1", "LAN IP detection failed; advertising 127.0.0.1."


def find_lan_ips(advertise_host: str) -> list[str]:
    ips = {advertise_host, "127.0.0.1"}
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                ips.add(ip)
    except OSError:
        pass
    try:
        ips.add(detect_lan_ip()[0])
    except OSError:
        pass
    return sorted(ips, key=lambda v: (v.startswith("127."), v))


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise EOFError
        chunks.extend(chunk)
    return bytes(chunks)


def read_frame(sock: socket.socket) -> tuple[int, bytes]:
    head = recv_exact(sock, 2)
    b0, b1 = head
    opcode = b0 & 0x0F
    masked = bool(b1 & 0x80)
    length = b1 & 0x7F
    if length == 126:
        length = struct.unpack("!H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", recv_exact(sock, 8))[0]
    if not masked:
        raise ProtocolError("client frame was not masked")
    mask = recv_exact(sock, 4)
    payload = bytearray(recv_exact(sock, length))
    for i, value in enumerate(payload):
        payload[i] = value ^ mask[i % 4]
    return opcode, bytes(payload)


def send_frame(sock: socket.socket, opcode: int, payload: bytes = b"") -> None:
    first = 0x80 | (opcode & 0x0F)
    length = len(payload)
    if length < 126:
        header = bytes((first, length))
    elif length <= 0xFFFF:
        header = bytes((first, 126)) + struct.pack("!H", length)
    else:
        header = bytes((first, 127)) + struct.pack("!Q", length)
    sock.sendall(header + payload)


def close_payload(code: int, reason: str = "") -> bytes:
    return struct.pack("!H", code) + reason.encode("utf-8")


class WebSocketSession:
    def __init__(self, sock: socket.socket, peer: tuple[str, int] | str) -> None:
        self.sock = sock
        self.peer = peer
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.started = time.monotonic()
        self.last_nonzero = 0.0
        self.frames = 0
        self.window: deque[float] = deque()
        self.seq_gaps = 0
        self.last_seq: int | None = None
        self.left = 0
        self.right = 0
        self.mood = 1

    def send(self, opcode: int, payload: bytes = b"") -> bool:
        try:
            with self.lock:
                send_frame(self.sock, opcode, payload)
            return True
        except OSError:
            self.stop.set()
            return False

    def close(self, code: int = 1000, reason: str = "") -> None:
        self.send(0x8, close_payload(code, reason))
        self.stop.set()

    def run(self) -> None:
        threads = [
            threading.Thread(target=self.telemetry_loop, daemon=True),
            threading.Thread(target=self.stats_loop, daemon=True),
        ]
        for thread in threads:
            thread.start()
        try:
            self.recv_loop()
        finally:
            self.stop.set()
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def recv_loop(self) -> None:
        while not self.stop.is_set():
            try:
                opcode, payload = read_frame(self.sock)
            except (EOFError, ConnectionError, OSError, ProtocolError):
                return
            if opcode == 0x8:
                self.close()
                return
            if opcode == 0x9:
                self.send(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode not in (0x1, 0x2):
                self.close(1003, "Unsupported")
                return
            if payload:
                self.handle_payload(payload)

    def handle_payload(self, payload: bytes) -> None:
        op = payload[0]
        if op == OP_CONTROL and len(payload) == 4:
            self.left = struct.unpack("b", payload[1:2])[0]
            self.right = struct.unpack("b", payload[2:3])[0]
            seq = payload[3]
            if self.last_seq is not None:
                expected = (self.last_seq + 1) & 0xFF
                if seq != expected:
                    delta = (seq - expected) & 0xFF
                    self.seq_gaps += delta if 0 < delta < 128 else 1
            self.last_seq = seq
            self.frames += 1
            now = time.monotonic()
            self.window.append(now)
            if self.left or self.right:
                self.last_nonzero = now
            return
        if op in (OP_TRIM, OP_MOOD, OP_GESTURE, OP_SOUND) and len(payload) == 3:
            if op == OP_MOOD:
                self.mood = payload[1]
            return

    def telemetry_loop(self) -> None:
        while not self.stop.wait(0.25):
            now = time.monotonic()
            wave = (math.sin((now - self.started) / 18.0) + 1.0) / 2.0
            millivolts = int(3700 + wave * 500)
            percent = max(0, min(100, int((millivolts - 3700) / 5)))
            if int((now - self.started) // 12) % 2 == 1:
                mood = 2 + (int((now - self.started) // 12) % 4)
            else:
                mood = self.mood
            recent_drive = now - self.last_nonzero < 0.75
            frame = bytearray(30)
            frame[0] = OP_TELEMETRY
            frame[3:5] = struct.pack(">H", millivolts)
            frame[5] = percent
            frame[6] = mood & 0xFF
            frame[7] = 0
            frame[8] = 1 if recent_drive else 0
            frame[9] = 2 if recent_drive else 0
            if not self.send(0x2, bytes(frame)):
                return

    def stats_loop(self) -> None:
        while not self.stop.wait(1.0):
            now = time.monotonic()
            while self.window and now - self.window[0] > 1.0:
                self.window.popleft()
            print(
                f"ctrl {len(self.window):2d}/s  L={self.left:+4d} R={self.right:+4d}  "
                f"seq_gaps={self.seq_gaps}  total={self.frames}",
                flush=True,
            )


class RobotSimHandler(http.server.BaseHTTPRequestHandler):
    server_version = "RobotSim/1.0"

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"{self.client_address[0]} - {fmt % args}", flush=True)

    def send_cors_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self) -> None:
        self.send_response(http.HTTPStatus.NO_CONTENT)
        self.send_cors_headers()
        self.end_headers()

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/pair.json":
            self.handle_pair_json()
            return
        if parsed.path == "/favicon.ico":
            self.send_response(http.HTTPStatus.NO_CONTENT)
            self.end_headers()
            return
        if parsed.path == "/ws":
            self.handle_ws(parsed)
            return
        if parsed.path != "/":
            self.send_error(http.HTTPStatus.NOT_FOUND)
            return
        body = self.server.html_body  # type: ignore[attr-defined]
        self.send_response(http.HTTPStatus.OK)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def handle_pair_json(self) -> None:
        body = json.dumps(
            {
                "ssid": SSID,
                "password": PASSWORD,
                "url": self.server.advertised_url,  # type: ignore[attr-defined]
                "port": self.server.server_port,
                "token": self.server.token,  # type: ignore[attr-defined]
            },
            separators=(",", ":"),
        ).encode("utf-8")
        self.send_response(http.HTTPStatus.OK)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_cors_headers()
        self.end_headers()
        self.wfile.write(body)

    def handle_ws(self, parsed: urllib.parse.ParseResult) -> None:
        if self.headers.get("Upgrade", "").lower() != "websocket":
            self.send_error(http.HTTPStatus.BAD_REQUEST, "Expected WebSocket upgrade")
            return
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_error(http.HTTPStatus.BAD_REQUEST, "Missing Sec-WebSocket-Key")
            return
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
        self.send_response(http.HTTPStatus.SWITCHING_PROTOCOLS)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()

        token = urllib.parse.parse_qs(parsed.query).get("k", [""])[0]
        required = self.server.require_token  # type: ignore[attr-defined]
        self.close_connection = True
        session = WebSocketSession(self.connection, self.client_address)
        if required and not secrets.compare_digest(token, required):
            session.close(CLOSE_POLICY_VIOLATION, "Forbidden")
            return
        print(f"ws connected {self.client_address[0]} token={token or '-'}", flush=True)
        session.run()
        print(f"ws disconnected {self.client_address[0]}", flush=True)


class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        server_address: tuple[str, int],
        handler_class: type[http.server.BaseHTTPRequestHandler],
        html_body: bytes,
        require_token: str | None,
        token: str,
        advertise_host: str,
    ) -> None:
        super().__init__(server_address, handler_class)
        self.html_body = html_body
        self.require_token = require_token
        self.token = token
        self.advertise_host = advertise_host
        self.advertised_url = f"http://{advertise_host}:{self.server_port}/?k={token}"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Serve the override phone app with a fake robot WebSocket.")
    parser.add_argument("--host", default="0.0.0.0", help="interface to bind (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8080, help="port to bind (default: 8080)")
    parser.add_argument("--token", default=DEFAULT_TOKEN, help="16-hex access token for printed URLs")
    parser.add_argument("--require-token", metavar="HEX", help="reject /ws tokens that do not match HEX")
    parser.add_argument("--advertise-host", help="host/IP to place in /pair.json and printed phone URL")
    args = parser.parse_args(argv)
    for name in ("token", "require_token"):
        value = getattr(args, name, None)
        if value and (len(value) != 16 or any(ch not in "0123456789abcdefABCDEF" for ch in value)):
            parser.error(f"--{name.replace('_', '-')} must be exactly 16 hex characters")
        if value:
            setattr(args, name, value.lower())
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        html = load_override_html()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    detected_host, detection_note = detect_lan_ip()
    advertise_host = args.advertise_host or detected_host
    httpd = ThreadingHTTPServer(
        (args.host, args.port),
        RobotSimHandler,
        html,
        args.require_token,
        args.token,
        advertise_host,
    )
    if args.host in ("0.0.0.0", "::"):
        urls = [f"http://{ip}:{args.port}/?k={args.token}" for ip in find_lan_ips(advertise_host)]
    else:
        urls = [f"http://{args.host}:{args.port}/?k={args.token}"]

    print("Override phone-app robot simulator", flush=True)
    print(f"Serving firmware HTML extracted from src\\override_html.cpp ({len(html)} bytes)", flush=True)
    print(f"Scannable URL: {httpd.advertised_url}", flush=True)
    if detection_note and not args.advertise_host:
        print(f"note: {detection_note}", flush=True)
    print("Open one of these URLs:", flush=True)
    for url in urls:
        print(f"  {url}", flush=True)
    print(f"Firmware AP SSID: {SSID}", flush=True)
    print(f"Firmware AP password: {PASSWORD}", flush=True)
    print(f"Real-device LCD QR encodes: http://192.168.4.1/?k={args.token}", flush=True)
    if args.require_token:
        print(f"WebSocket token enforcement: require {args.require_token}", flush=True)
    else:
        print("WebSocket token enforcement: disabled (any token accepted)", flush=True)
    print("Press Ctrl+C to stop.", flush=True)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping.")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
