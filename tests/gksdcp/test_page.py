"""The optional web page on the web port (WEBUI=1) and its absence in a WEBUI=0 build."""
import json
import os
import shutil
import socket
import subprocess
import urllib.error
import urllib.request

import conftest
from sdcp_client import read_http_response
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
GKSDCP = os.path.join(HERE, "..", "..", "gksdcp")

IDS = ["status-state", "status-file", "status-layer", "status-progress", "status-elapsed",
       "status-remaining", "btn-pause", "btn-resume", "btn-stop", "file-list", "tab-local", "tab-usb",
       "upload-input", "upload-progress", "message", "btn-upload-cancel", "free-space",
       "btn-z-up1", "btn-z-home", "btn-z-stop"]


def get(url):
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return r.status, r.headers.get("Content-Type", ""), r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("Content-Type", ""), e.read()


def test_index_served(gksdcp_start):
    srv = gksdcp_start()
    code, ctype, body = get("http://127.0.0.1:%d/" % srv.web_port)
    assert code == 200
    assert ctype == "text/html; charset=utf-8"
    for i in IDS:
        assert ('id="%s"' % i).encode() in body, i
    assert b"<title>GKtwo</title>" in body
    assert b"Printer app not reachable" in body
    assert len(body) < 20 * 1024


def test_no_external_urls(gksdcp_start):
    srv = gksdcp_start()
    body = get("http://127.0.0.1:%d/" % srv.web_port)[2]
    for bad in (b"http://", b"https://", b"//cdn"):
        assert bad not in body


def test_other_paths_404(gksdcp_start):
    srv = gksdcp_start()
    base = "http://127.0.0.1:%d" % srv.web_port
    for p in ("/api/status", "/index.html", "/websocket", "/uploadFile/upload", "/x"):
        assert get(base + p)[0] == 404, p


def test_web_port_off(gksdcp_start):
    # the fixture passes -w <port>; a later -w 0 wins
    srv = gksdcp_start(["-w", "0"])
    with pytest.raises(OSError):
        socket.create_connection(("127.0.0.1", srv.web_port), 0.5).close()


def test_upload_failure_is_readable_cross_origin(gksdcp_start):
    # the page on port 80 reads status and body of a POST to port 3030; the browser needs a CORS header for that
    srv = gksdcp_start()
    req = urllib.request.Request("http://127.0.0.1:%d/uploadFile/upload" % srv.ws_port, data=b"x", method="POST",
                                 headers={"Origin": "http://127.0.0.1", "Content-Type": "text/plain"})
    with pytest.raises(urllib.error.HTTPError) as e:
        urllib.request.urlopen(req, timeout=5)
    assert e.value.code == 400
    assert e.value.headers.get("Access-Control-Allow-Origin") == "http://127.0.0.1"
    assert e.value.headers.get("Vary") == "Origin"
    assert json.loads(e.value.read())["success"] is False


def test_upload_reply_without_or_foreign_origin_has_no_cors_header(gksdcp_start):
    srv = gksdcp_start()
    url = "http://127.0.0.1:%d/uploadFile/upload" % srv.ws_port
    for origin in (None, "http://evil.example"):
        headers = {"Content-Type": "text/plain"}
        if origin:
            headers["Origin"] = origin
        req = urllib.request.Request(url, data=b"x", method="POST", headers=headers)
        with pytest.raises(urllib.error.HTTPError) as e:
            urllib.request.urlopen(req, timeout=5)
        assert e.value.code in (400, 403)
        assert e.value.headers.get("Access-Control-Allow-Origin") is None, origin


def test_page_reply_cannot_be_framed(gksdcp_start):
    srv = gksdcp_start()
    with urllib.request.urlopen("http://127.0.0.1:%d/" % srv.web_port, timeout=5) as r:
        assert r.headers.get("X-Frame-Options") == "DENY"
        assert r.headers.get("X-Content-Type-Options") == "nosniff"


@pytest.mark.skipif(shutil.which("make") is None, reason="make not installed")
def test_webui_off_build_has_no_web_listener(tmp_path, mock_hook):
    subprocess.run(["make", "-C", GKSDCP, "WEBUI=0", "BUILD=build-noweb", "build-noweb/gksdcp-host"],
                   check=True, capture_output=True)
    binary = os.path.join(GKSDCP, "build-noweb", "gksdcp-host")
    ports = [conftest._free_port() for _ in range(3)]
    for d in ("files", "usb", "var"):
        (tmp_path / d).mkdir()
    # -w asks for a web port; the build without the page ignores it
    proc = subprocess.Popen([binary, "-p", str(ports[0]), "-u", str(ports[1]), "-w", str(ports[2]),
                             "-d", str(tmp_path / "files"), "-U", str(tmp_path / "usb"), "-v", str(tmp_path / "var"),
                             "-s", mock_hook.sock_path, "-m", "0000aabbccddeeff", "-T"], stdin=subprocess.DEVNULL)
    try:
        conftest._wait_listening(proc, ports[0])
        with pytest.raises(OSError):
            socket.create_connection(("127.0.0.1", ports[2]), 0.5).close()
    finally:
        proc.terminate()
        proc.wait(5)


def _raw_upload_post(port, host, origin):
    s = socket.create_connection(("127.0.0.1", port), 5)
    s.sendall(("POST /uploadFile/upload HTTP/1.1\r\nHost: %s\r\nOrigin: %s\r\nContent-Type: text/plain\r\n"
               "Content-Length: 1\r\nConnection: close\r\n\r\nx" % (host, origin)).encode())
    try:
        return read_http_response(s, 5)
    finally:
        s.close()


def test_longest_allowed_origin_gets_whole_cors_headers(gksdcp_start):
    # Host is limited to 127 bytes and an allowed Origin is http://<host> or http://<host>:80, so 137 bytes is
    # the longest Origin that is echoed: the ACAO and Vary lines must arrive whole and the body must still parse
    srv = gksdcp_start()
    host = "h" * 127
    origin = "http://" + host + ":80"
    status, headers, body = _raw_upload_post(srv.ws_port, host, origin)
    assert status == 400
    assert headers.get("access-control-allow-origin") == origin
    assert headers.get("vary") == "Origin"
    assert headers.get("x-frame-options") == "DENY" and headers.get("x-content-type-options") == "nosniff"
    assert json.loads(body)["success"] is False


def test_longest_stored_origin_foreign_is_refused_cleanly(gksdcp_start):
    # a 159-byte Origin fits the parser slot; it is not the page's origin, so there is no CORS echo
    srv = gksdcp_start()
    origin = "http://" + "e" * 152
    status, headers, body = _raw_upload_post(srv.ws_port, "x", origin)
    assert status == 403
    assert "access-control-allow-origin" not in headers
    assert json.loads(body)["success"] is False
