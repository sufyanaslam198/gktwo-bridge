"""Connection churn: DragonFruit-style short connections, abrupt closes and aborted uploads must not leak
file descriptors, memory or temp files."""
import os
import socket
import struct
import time

from sdcp_client import build_multipart, upload_chunk, ws_connect


def _fds(pid):
    return len(os.listdir("/proc/%d/fd" % pid))


def _rss_kb(pid):
    with open("/proc/%d/status" % pid) as f:
        for ln in f:
            if ln.startswith("VmRSS:"):
                return int(ln.split()[1])
    raise AssertionError("no VmRSS")


def _dragonfruit(port):
    """Connect, text 'ping', Cmd 0, read the answer, close."""
    ws = ws_connect(port)
    ws.send_text("ping")
    rid = ws.request(0)
    ws.wait_response(rid)
    ws.close()


def _abrupt_mid_frame(port):
    """Handshake, then the start of a masked text frame (header and part of the payload), then a hard close."""
    ws = ws_connect(port)
    payload = b'{"Id":"x","Data":{"Cmd":0,'
    ws.sock.sendall(struct.pack("!BB", 0x81, 0x80 | 200) + b"\x01\x02\x03\x04" + payload)
    ws.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))   # RST, not FIN
    ws.sock.close()


def _aborted_upload(port, n):
    """A distinct Uuid per upload. Odd ones complete a first chunk, then the second (Offset > 0) is cut off
    mid-body; even ones are cut off in their first chunk. While an earlier abort is under 5 s old the server
    refuses a new Uuid with 409, which also must not leak anything."""
    uuid = "%08x-0d4a-4c55-9a7e-6b1f2d3c4e5f" % (0x3f2b8c1e + n)
    name = "churn%d.ctb" % n
    offset = 0
    if n % 2:
        st, js = upload_chunk(port, {"S-File-MD5": "", "Check": "0", "Offset": 0, "Uuid": uuid, "TotalSize": 80000},
                              name, b"y" * 40000)
        if st == 409:                  # an earlier abort still holds the session (takeover needs 5 s of quiet)
            return
        assert st == 200 and js["success"] is True, (st, js)
        offset = 40000
    fields = {"S-File-MD5": "", "Check": "0", "Offset": offset, "Uuid": uuid, "TotalSize": 80000}
    boundary, body = build_multipart(fields, name, b"x" * 40000)
    head = ("POST /uploadFile/upload HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n" % (boundary, len(body))).encode()
    s = socket.create_connection(("127.0.0.1", port), 5)
    s.sendall(head + body[:len(body) // 2])
    time.sleep(0.02)
    s.close()


def _wait_gone(srv, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if not [p for p in os.listdir(srv.files) if p.endswith(".part")]:
            return True
        time.sleep(0.2)
    return False


def test_churn_leaks_nothing(gksdcp_start):
    srv = gksdcp_start(("-S", "2"))
    for _ in range(20):                          # warm up: first-use allocations are not a leak
        _dragonfruit(srv.ws_port)
    time.sleep(0.5)
    fds0, rss0 = _fds(srv.pid), _rss_kb(srv.pid)

    for _ in range(300):
        _dragonfruit(srv.ws_port)
    for _ in range(50):
        _abrupt_mid_frame(srv.ws_port)
    for i in range(20):
        if i in (1, 11):               # let the previous abort age out so this one starts a session, Offset > 0 included
            time.sleep(5.3)
        _aborted_upload(srv.ws_port, i)

    assert srv.proc.poll() is None, "gksdcp died"
    assert _wait_gone(srv, 6.0), "a .part file outlived the stale timeout: %s" % os.listdir(srv.files)
    time.sleep(0.5)
    fds1, rss1 = _fds(srv.pid), _rss_kb(srv.pid)
    assert abs(fds1 - fds0) <= 2, "fd count %d -> %d" % (fds0, fds1)
    assert rss1 - rss0 < 512, "RSS grew %d KB" % (rss1 - rss0)
    ws = ws_connect(srv.ws_port)                 # still serving
    ws.wait_response(ws.request(0))
    ws.close()
