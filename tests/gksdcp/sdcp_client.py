"""Stdlib-only SDCP test client: WebSocket, UDP discovery, chunked upload."""
import base64
import json
import os
import socket
import struct
import time
import uuid

DEFAULT_MAINBOARD_ID = "0000aabbccddeeff"


class HandshakeError(Exception):
    def __init__(self, status, headers, body=b""):
        super().__init__("handshake refused: HTTP %d" % status)
        self.status = status
        self.headers = headers
        self.body = body


def _read_head(sock, timeout):
    sock.settimeout(timeout)
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split()[1]) if lines and lines[0].startswith("HTTP/") else 0
    headers = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(":")
        headers[k.strip().lower()] = v.strip()
    return status, headers, rest


class WS:
    def __init__(self, sock, leftover=b"", mainboard_id=DEFAULT_MAINBOARD_ID):
        self.sock = sock
        self.buf = leftover
        self.mainboard_id = mainboard_id
        self.msgs = []          # parsed JSON text messages not yet consumed

    # ---- raw frames ----
    def send_frame(self, op, payload=b"", fin=True, mask=True):
        b0 = (0x80 if fin else 0) | op
        n = len(payload)
        m = 0x80 if mask else 0
        if n < 126:
            head = struct.pack("!BB", b0, m | n)
        elif n < 65536:
            head = struct.pack("!BBH", b0, m | 126, n)
        else:
            head = struct.pack("!BBQ", b0, m | 127, n)
        if mask:
            key = os.urandom(4)
            payload = bytes(c ^ key[i & 3] for i, c in enumerate(payload))
            head += key
        self.sock.sendall(head + payload)

    def send_text(self, s):
        self.send_frame(1, s.encode() if isinstance(s, str) else s)

    def send_ping(self, b=b""):
        self.send_frame(9, b)

    def _need(self, n, deadline):
        while len(self.buf) < n:
            left = deadline - time.monotonic()
            if left <= 0:
                raise socket.timeout("timed out")
            self.sock.settimeout(left)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed by peer")
            self.buf += chunk

    def recv(self, timeout=3.0):
        """Next server frame as (opcode, payload). Raises socket.timeout / ConnectionError."""
        deadline = time.monotonic() + timeout
        self._need(2, deadline)
        op = self.buf[0] & 0x0F
        ln = self.buf[1] & 0x7F
        off = 2
        if ln == 126:
            self._need(4, deadline)
            ln = struct.unpack("!H", self.buf[2:4])[0]
            off = 4
        elif ln == 127:
            self._need(10, deadline)
            ln = struct.unpack("!Q", self.buf[2:10])[0]
            off = 10
        self._need(off + ln, deadline)
        payload = self.buf[off:off + ln]
        self.buf = self.buf[off + ln:]
        return op, payload

    # ---- SDCP helpers ----
    def request(self, cmd, data=None, mainboard_id=None, rid=None):
        mid = mainboard_id or self.mainboard_id
        rid = rid or uuid.uuid4().hex
        msg = {
            "Id": uuid.uuid4().hex,
            "Data": {
                "Cmd": cmd,
                "Data": data if data is not None else {},
                "RequestID": rid,
                "MainboardID": mid,
                "TimeStamp": int(time.time() * 1000),
                "From": 0,
            },
            "Topic": "sdcp/request/" + mid,
        }
        self.send_text(json.dumps(msg))
        return rid

    def _wait(self, pred, timeout):
        deadline = time.monotonic() + timeout
        while True:
            for i, m in enumerate(self.msgs):
                if pred(m):
                    return self.msgs.pop(i)
            left = deadline - time.monotonic()
            if left <= 0:
                raise socket.timeout("no matching message")
            op, payload = self.recv(left)
            if op == 1:
                try:
                    self.msgs.append(json.loads(payload))
                except ValueError:
                    pass        # e.g. the text "pong"
            elif op == 8:
                raise ConnectionError("close frame received")

    def wait_response(self, rid, timeout=3.0):
        return self._wait(lambda m: m.get("Topic", "").startswith("sdcp/response/")
                          and m.get("Data", {}).get("RequestID") == rid, timeout)

    def wait_topic(self, kind, timeout=3.0):
        return self._wait(lambda m: m.get("Topic", "").startswith("sdcp/%s/" % kind), timeout)

    def close(self):
        try:
            self.send_frame(8, struct.pack("!H", 1000))
        except OSError:
            pass
        self.sock.close()


def ws_connect(port, origin=None, path="/websocket", host="127.0.0.1", timeout=3.0, greeting=True):
    """greeting=True reads and drops the attributes and status frames sent to every new connection."""
    sock = socket.create_connection((host, port), timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    lines = ["GET %s HTTP/1.1" % path, "Host: %s:%d" % (host, port), "Upgrade: websocket",
             "Connection: Upgrade", "Sec-WebSocket-Key: " + key, "Sec-WebSocket-Version: 13"]
    if origin is not None:
        lines.append("Origin: " + origin)
    sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
    status, headers, rest = _read_head(sock, timeout)
    if status != 101:
        sock.close()
        raise HandshakeError(status, headers, rest)
    ws = WS(sock, rest)
    if greeting:
        ws.wait_topic("attributes", timeout)
        ws.wait_topic("status", timeout)
    return ws


def discover(port, addr="127.0.0.1", timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(b"M99999", (addr, port))
        data, _ = s.recvfrom(4096)
    finally:
        s.close()
    return json.loads(data)


def build_multipart(fields, filename, data, boundary=None):
    """(boundary, body) for one chunk: the fields in dict order, then the File part."""
    boundary = boundary or "----gktest" + uuid.uuid4().hex
    body = b""
    for k, v in fields.items():
        body += ('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n'
                 % (boundary, k, v)).encode()
    body += ('--%s\r\nContent-Disposition: form-data; name="File"; filename="%s"\r\n'
             'Content-Type: application/octet-stream\r\n\r\n' % (boundary, filename)).encode()
    body += data + ("\r\n--%s--\r\n" % boundary).encode()
    return boundary, body


def read_http_response(sock, timeout):
    """(status, headers, body bytes) of one response; the body is read by Content-Length."""
    status, headers, rest = _read_head(sock, timeout)
    want = int(headers.get("content-length", "0"))
    sock.settimeout(timeout)
    while len(rest) < want:
        chunk = sock.recv(4096)
        if not chunk:
            break
        rest += chunk
    return status, headers, rest


def upload_chunk(port, fields, filename, data, origin=None, host="127.0.0.1", timeout=5.0, extra_headers=()):
    """POST one multipart chunk to /uploadFile/upload. Returns (http_status, json dict)."""
    boundary, body = build_multipart(fields, filename, data)
    head = ["POST /uploadFile/upload HTTP/1.1", "Host: %s:%d" % (host, port),
            "Content-Type: multipart/form-data; boundary=" + boundary,
            "Content-Length: %d" % len(body), "Connection: close"]
    if origin is not None:
        head.append("Origin: " + origin)
    head.extend(extra_headers)
    sock = socket.create_connection((host, port), timeout)
    try:
        sock.sendall(("\r\n".join(head) + "\r\n\r\n").encode("utf-8") + body)
        status, _, rest = read_http_response(sock, timeout)
    finally:
        sock.close()
    try:
        return status, json.loads(rest)
    except ValueError:
        return status, {}
