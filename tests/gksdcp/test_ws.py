import json
import socket
import struct
import time

import pytest

from sdcp_client import HandshakeError, ws_connect


def expect_closed(ws, timeout=3):
    """Drain frames until the peer closes; only a close frame may precede EOF."""
    with pytest.raises(ConnectionError):
        while True:
            op, _ = ws.recv(timeout)
            assert op == 8


def test_handshake_ok(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    ws.close()


def test_handshake_wrong_path_404(gksdcp_start):
    srv = gksdcp_start()
    with pytest.raises(HandshakeError) as e:
        ws_connect(srv.ws_port, path="/nope")
    assert e.value.status == 404


def test_handshake_foreign_origin_403(gksdcp_start):
    srv = gksdcp_start()
    with pytest.raises(HandshakeError) as e:
        ws_connect(srv.ws_port, origin="http://evil.example")
    assert e.value.status == 403
    # same-host origin (no port, and :80) is accepted
    ws_connect(srv.ws_port, origin="http://127.0.0.1").close()
    ws_connect(srv.ws_port, origin="http://127.0.0.1:80").close()


def test_upload_path_without_multipart_is_a_failure_reply(gksdcp_start):
    srv = gksdcp_start()
    s = socket.create_connection(("127.0.0.1", srv.ws_port), 3)
    s.sendall(b"POST /uploadFile/upload HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
    assert s.recv(64).startswith(b"HTTP/1.1 400")
    s.close()


def test_malformed_head_400_and_closed(gksdcp_start):
    srv = gksdcp_start()
    s = socket.create_connection(("127.0.0.1", srv.ws_port), 3)
    s.sendall(b"NOT HTTP AT ALL\r\n\r\n")
    data = b""
    while True:                     # the server answers, then closes: read to EOF
        chunk = s.recv(256)
        if not chunk:
            break
        data += chunk
    assert data.startswith(b"HTTP/1.1 400")
    s.close()


def test_protocol_ping_gets_pong(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    ws.send_ping(b"heartbeat")
    assert ws.recv(2) == (10, b"heartbeat")
    ws.close()


def test_new_connection_gets_attributes_then_status(gksdcp_start):
    # DragonFruit learns the MainboardID this way when UDP discovery cannot reach the printer:
    # it sends "ping" on a new connection and reads frames for about 1.8 s.
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port, greeting=False)
    ws.send_text("ping")
    frames, deadline = [], time.monotonic() + 1.0
    while len(frames) < 2:
        op, payload = ws.recv(max(0.01, deadline - time.monotonic()))
        if op == 1 and payload != b"pong":
            frames.append(json.loads(payload))
    assert [f["Topic"] for f in frames] == ["sdcp/attributes/0000aabbccddeeff", "sdcp/status/0000aabbccddeeff"]
    assert all(f["MainboardID"] == "0000aabbccddeeff" for f in frames)
    assert frames[0]["Attributes"]["MainboardID"] == "0000aabbccddeeff"
    assert frames[0]["Attributes"]["Name"] == "GKtwo"
    ws.close()


def test_text_ping_gets_text_pong(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    ws.send_text("ping")
    assert ws.recv(2) == (1, b"pong")
    ws.close()


def test_close_frame_echoed(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    ws.send_frame(8, struct.pack("!H", 1000))
    op, payload = ws.recv(2)
    assert op == 8 and payload[:2] == struct.pack("!H", 1000)
    with pytest.raises(ConnectionError):
        ws.recv(2)
    ws.sock.close()


def test_oversize_frame_closes_only_that_conn(gksdcp_start):
    srv = gksdcp_start()
    good = ws_connect(srv.ws_port)
    bad = ws_connect(srv.ws_port)
    # masked header announcing 70000 bytes (> 65536); the payload never needs to arrive
    bad.sock.sendall(struct.pack("!BBQ", 0x81, 0x80 | 127, 70000) + b"\x00\x00\x00\x00")
    expect_closed(bad)
    good.send_text("ping")
    assert good.recv(2) == (1, b"pong")
    good.close()


def test_unmasked_frame_closes_conn(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    ws.send_frame(1, b"hi", mask=False)
    expect_closed(ws)


def test_33rd_connection_refused_others_fine(gksdcp_start):
    srv = gksdcp_start()
    conns = [ws_connect(srv.ws_port) for _ in range(32)]
    extra = socket.create_connection(("127.0.0.1", srv.ws_port), 3)
    try:
        try:
            extra.sendall(b"GET /websocket HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                          b"Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n")
            extra.settimeout(3)
            data = extra.recv(256)
        except ConnectionError:
            data = b""
        assert data.startswith(b"HTTP/1.1 503")
    finally:
        extra.close()
    for ws in (conns[0], conns[-1]):
        ws.send_text("ping")
        assert ws.recv(2) == (1, b"pong")
    # a slot frees up again once someone leaves
    conns.pop().close()
    time.sleep(0.3)
    ws_connect(srv.ws_port).close()
    for ws in conns:
        ws.close()


def test_error_reply_to_streaming_post_is_readable(gksdcp_start):
    # the failure reply goes out after the head; the client keeps streaming 2 MB and must still read it
    srv = gksdcp_start()
    s = socket.create_connection(("127.0.0.1", srv.ws_port), 5)
    s.sendall(b"POST /uploadFile/upload HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n")
    chunk = b"z" * 65536
    sent = 0
    while sent < 2000000:
        s.sendall(chunk[:min(65536, 2000000 - sent)])
        sent += 65536
    data = b""
    while True:
        part = s.recv(1024)
        if not part:
            break
        data += part
    assert data.startswith(b"HTTP/1.1 400") and b'"success":false' in data
    s.close()


def rss_kb(pid):
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])


def test_backpressure_bounds_memory_and_others_fine(gksdcp_start):
    srv = gksdcp_start()
    good = ws_connect(srv.ws_port)
    flooders = []
    frame = None
    for _ in range(16):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)   # tiny window: replies pile up in the server
        s.connect(("127.0.0.1", srv.ws_port))
        s.sendall(b"GET /websocket HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n")
        assert s.recv(256).startswith(b"HTTP/1.1 101")
        s.setblocking(False)
        flooders.append(s)
    # masked 125-byte ping
    key = b"\x01\x02\x03\x04"
    frame = b"\x89\xfd" + key + bytes(c ^ key[i & 3] for i, c in enumerate(b"p" * 125))
    blob = frame * 4000                                            # ~520 KB per refill
    todo = [blob] * len(flooders)
    peak = 0
    for _ in range(60):
        for i, s in enumerate(flooders):
            try:
                n = s.send(todo[i])                                # never read the pongs
                todo[i] = todo[i][n:] or blob                      # keep frames aligned after partial sends
            except (BlockingIOError, ConnectionError):
                pass
        time.sleep(0.05)
        peak = max(peak, rss_kb(srv.pid))      # sample during the flood: an over-cap conn is closed and frees its queue
    assert peak < 4608                          # without backpressure this reaches ~6 MB
    good.send_text("ping")
    assert good.recv(3) == (1, b"pong")
    good.close()
    for s in flooders:
        s.close()
