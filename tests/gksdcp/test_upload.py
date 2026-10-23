import hashlib
import os
import shutil
import socket
import tempfile
import time

import pytest

from sdcp_client import build_multipart, read_http_response, upload_chunk, ws_connect
from test_files import _ack, _start, _touch
from test_print import _notice
from test_status import settle

CHUNK = 1 << 20
UUID = "3f2b8c1e-0d4a-4c55-9a7e-6b1f2d3c4e5f"


def _fields(offset, total, uuid=UUID, md5="", check="0"):
    return {"S-File-MD5": md5, "Check": check, "Offset": offset, "Uuid": uuid, "TotalSize": total}


def _send(srv, name, data, offset, total, **kw):
    return upload_chunk(srv.ws_port, _fields(offset, total, **kw), name, data)


def _upload_all(srv, name, data, chunk=CHUNK, **kw):
    """DragonFruit style: sequential chunks, same Uuid. Returns the last (status, json)."""
    res = None
    for off in range(0, len(data), chunk):
        res = _send(srv, name, data[off:off + chunk], off, len(data), **kw)
        assert res[0] == 200 and res[1]["success"] is True, res
    return res


def _fail_codes(js):
    assert js["success"] is False and js["code"] == "111111" and js["data"] is None
    return js["messages"]


def _st(res, want):
    assert res[0] == want, res
    return res[1]


def _code(js):
    return _fail_codes(js)[0]["message"]


def _parts(srv):
    return sorted(p.name for p in srv.files.iterdir() if p.name.endswith(".part"))


def _wait_status(ws, want, timeout=3.0):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            last = ws.wait_topic("status", max(0.05, end - time.monotonic()))
        except socket.timeout:
            break
        if last["Status"]["CurrentStatus"] == want:
            return last
    raise AssertionError("status never became %s (last %s)" % (want, last and last["Status"]["CurrentStatus"]))


def _raw_post(srv, headers, body, timeout=5.0):
    s = socket.create_connection(("127.0.0.1", srv.ws_port), timeout)
    s.sendall(("\r\n".join(["POST /uploadFile/upload HTTP/1.1", "Host: x"] + headers) + "\r\n\r\n").encode() + body)
    try:
        return read_http_response(s, timeout)
    finally:
        s.close()


def test_head_probe_200(gksdcp_start):
    srv = gksdcp_start()
    for extra in ("", "Origin: http://evil.example\r\n"):
        s = socket.create_connection(("127.0.0.1", srv.ws_port), 3)
        s.sendall(("HEAD /uploadFile/upload HTTP/1.1\r\nHost: x\r\n" + extra + "\r\n").encode())
        status, headers, rest = read_http_response(s, 3)
        assert status == 200 and headers["content-length"] == "0" and rest == b""
        s.close()


def test_dragonfruit_style_upload(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    data = os.urandom(3 * CHUNK + CHUNK // 2)
    status, js = _send(srv, "job.ctb", data[:CHUNK], 0, len(data))
    assert status == 200 and js == {"code": "000000", "messages": None, "data": {}, "success": True}
    for off in range(CHUNK, len(data), CHUNK):
        status, js = _send(srv, "job.ctb", data[off:off + CHUNK], off, len(data))
        assert status == 200 and js["success"] is True
    assert (srv.files / "job.ctb").read_bytes() == data
    assert _parts(srv) == []
    d = _ack(ws, 258, {"Url": "/local/"})
    assert [e["name"] for e in d["FileList"]] == ["/local/job.ctb"]
    assert d["FileList"][0]["usedSize"] > 0
    ws.close()


def test_md5_checked_ok_and_mismatch(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    data = os.urandom(CHUNK + 1000)
    md5 = hashlib.md5(data).hexdigest()
    _upload_all(srv, "ok.ctb", data, md5=md5.upper(), check="1", uuid="u-ok")
    assert (srv.files / "ok.ctb").read_bytes() == data
    settle(ws)
    bad = "0" * 32
    _send(srv, "bad.ctb", data[:CHUNK], 0, len(data), md5=bad, check="1", uuid="u-bad")
    status, js = _send(srv, "bad.ctb", data[CHUNK:], CHUNK, len(data), md5=bad, check="1", uuid="u-bad")
    assert status == 500 and _code(js) == -4
    assert {"field": "common_field", "message": "md5 mismatch"} in js["messages"]
    assert not (srv.files / "bad.ctb").exists() and _parts(srv) == []
    err = ws.wait_topic("error")
    assert err["Data"]["Data"]["ErrorCode"] == 1
    # Check "1" with an empty MD5 is not verified
    _upload_all(srv, "nomd5.ctb", data, md5="", check="1", uuid="u-no")
    assert (srv.files / "nomd5.ctb").exists()
    ws.close()


def test_offset_mismatch_minus2(gksdcp_start):
    srv = gksdcp_start()
    assert _send(srv, "a.ctb", b"x" * 100, 0, 300)[1]["success"] is True
    assert _code(_st(_send(srv, "a.ctb", b"x" * 100, 50, 300), 400)) == -2       # not where the file ends
    assert _code(_st(_send(srv, "a.ctb", b"x" * 100, 200, 300), 400)) == -2
    assert _send(srv, "a.ctb", b"x" * 100, 100, 300)[1]["success"] is True   # still resumable
    assert _send(srv, "a.ctb", b"x" * 100, 200, 300)[1]["success"] is True
    assert (srv.files / "a.ctb").read_bytes() == b"x" * 300
    # a first chunk that does not start at 0
    assert _code(_st(_send(srv, "b.ctb", b"x" * 10, 5, 300, uuid="other"), 400)) == -2
    assert _parts(srv) == []


def test_negative_offset_minus1(gksdcp_start):
    srv = gksdcp_start()
    status, js = _send(srv, "a.ctb", b"x" * 10, -1, 300)
    assert status == 400 and _code(js) == -1
    assert _parts(srv) == []


def test_second_uuid_refused(gksdcp_start):
    srv = gksdcp_start()
    assert _send(srv, "a.ctb", b"x" * 100, 0, 300)[1]["success"] is True
    status, js = _send(srv, "b.ctb", b"y" * 100, 0, 300, uuid="another-one")
    assert status == 409 and _code(js) == -4
    assert {"field": "common_field", "message": "another upload is running"} in js["messages"]
    assert not (srv.files / "b.ctb").exists()
    assert len(_parts(srv)) == 1
    _send(srv, "a.ctb", b"x" * 200, 100, 300)
    assert (srv.files / "a.ctb").exists()
    # free again afterwards
    assert _send(srv, "b.ctb", b"y" * 300, 0, 300, uuid="another-one")[1]["success"] is True


def test_refused_while_printing(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    mock_hook.state.update(state="printing", file="/media/emmc/job.ctb", layer=1, total_layers=10, phase="exposing")
    time.sleep(0.7)
    status, js = _send(srv, "new.ctb", b"x" * 10, 0, 10)
    assert status == 409 and _code(js) == -4
    assert {"field": "common_field", "message": "printer busy"} in js["messages"]
    mock_hook.state.update(state="paused")
    time.sleep(0.7)
    assert _code(_st(_send(srv, "new.ctb", b"x" * 10, 0, 10), 409)) == -4
    assert not (srv.files / "new.ctb").exists() and _parts(srv) == []
    mock_hook.state.update(state="complete")
    time.sleep(0.7)
    assert _send(srv, "new.ctb", b"x" * 10, 0, 10)[1]["success"] is True
    ws.close()


def test_replace_refused_while_start_pending(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "a.ctb", data=b"old")
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 0       # start queued, snapshot still idle
    status, js = _send(srv, "a.ctb", b"new" * 10, 0, 30)
    assert status == 409 and _code(js) == -4
    assert {"field": "common_field", "message": "printer busy"} in js["messages"]
    assert (srv.files / "a.ctb").read_bytes() == b"old" and _parts(srv) == []
    assert _send(srv, "fresh.ctb", b"n" * 10, 0, 10)[1]["success"] is True    # a new name is not a replace
    ws.close()


def test_replace_existing_when_idle(gksdcp_start):
    srv = gksdcp_start()
    _touch(srv.files / "a.ctb", data=b"old")
    assert _send(srv, "a.ctb", b"new" * 10, 0, 30)[1]["success"] is True
    assert (srv.files / "a.ctb").read_bytes() == b"new" * 10


def test_replace_refused_in_state_starting(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "a.ctb", data=b"old")
    mock_hook.state.update(state="starting", file="/media/emmc/a.ctb")
    time.sleep(0.7)
    assert _code(_st(_send(srv, "a.ctb", b"new" * 10, 0, 30), 409)) == -4
    assert (srv.files / "a.ctb").read_bytes() == b"old"
    assert _send(srv, "fresh.ctb", b"n" * 10, 0, 10)[1]["success"] is True   # a new name is not a replace
    ws.close()


@pytest.mark.parametrize("name", ["../x.ctb", "a.txt", "", ".hidden.ctb", "a/b.ctb", "a\\b.ctb", "noext", "x.ctb.part"])
def test_bad_names_refused(gksdcp_start, name):
    srv = gksdcp_start()
    status, js = _send(srv, name, b"x" * 10, 0, 10)
    assert status == 400 and _code(js) == -4
    assert {"field": "filename", "message": "invalid file name"} in js["messages"]
    assert sorted(p.name for p in srv.files.iterdir()) == []
    assert not (srv.files.parent / "x.ctb").exists()


def test_uppercase_extension_ok(gksdcp_start):
    srv = gksdcp_start()
    assert _send(srv, "UP.CTB", b"x" * 10, 0, 10)[1]["success"] is True
    assert (srv.files / "UP.CTB").exists()


def test_upload_unicode_name(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    name = "Drache_Ü (2).ctb"
    data = b"\x01" * 5000
    assert _send(srv, name, data, 0, len(data))[1]["success"] is True
    assert (srv.files / name).read_bytes() == data
    d = _ack(ws, 258, {"Url": "/local/"})
    assert [e["name"] for e in d["FileList"]] == ["/local/" + name]
    mock_hook.received.clear()
    assert _ack(ws, 128, {"Filename": "/local/" + name})["Ack"] == 0
    starts = [m for m in mock_hook.received if m.get("cmd") == "start"]
    assert starts == [{"cmd": "start", "path": "%s/%s" % (srv.files, name)}]
    ws.close()


def test_cmd255_cancel_removes_partial(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 255, {"Uuid": UUID, "FileName": "a.ctb"})["Ack"] == 1      # nothing is transferring
    assert _send(srv, "a.ctb", b"x" * 100, 0, 300)[1]["success"] is True
    assert len(_parts(srv)) == 1
    assert _ack(ws, 255, {"Uuid": "nope", "FileName": "a.ctb"})["Ack"] == 3
    assert _ack(ws, 255, {"Uuid": UUID, "FileName": "other.ctb"})["Ack"] == 3
    assert len(_parts(srv)) == 1
    assert _ack(ws, 255, {"Uuid": UUID, "FileName": "a.ctb"})["Ack"] == 0
    assert _parts(srv) == [] and not (srv.files / "a.ctb").exists()
    _wait_status(ws, [0])
    assert _ack(ws, 255, {"Uuid": UUID, "FileName": "a.ctb"})["Ack"] == 1
    # the next chunk of the cancelled upload is a fresh first chunk, and offset 100 is wrong
    assert _code(_st(_send(srv, "a.ctb", b"x" * 100, 100, 300), 400)) == -2
    ws.close()


def test_status_2_while_uploading(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    assert _send(srv, "a.ctb", b"x" * 100, 0, 200)[1]["success"] is True
    _wait_status(ws, [2])
    assert _send(srv, "a.ctb", b"x" * 100, 100, 200)[1]["success"] is True
    _wait_status(ws, [0])
    ws.close()


def test_cmd259_refused_while_upload_open(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "keep.ctb")
    assert _send(srv, "a.ctb", b"x" * 100, 0, 200)[1]["success"] is True
    assert _ack(ws, 259, {"FileList": ["/local/keep.ctb"]})["Ack"] == 1
    assert (srv.files / "keep.ctb").exists()
    assert _send(srv, "a.ctb", b"x" * 100, 100, 200)[1]["success"] is True
    assert _ack(ws, 259, {"FileList": ["/local/keep.ctb"]})["Ack"] == 0
    ws.close()


def test_foreign_origin_403(gksdcp_start):
    srv = gksdcp_start()
    status, js = upload_chunk(srv.ws_port, _fields(0, 10), "a.ctb", b"x" * 10, origin="http://evil.example")
    assert status == 403 and _code(js) == -4
    assert sorted(p.name for p in srv.files.iterdir()) == []
    status, js = upload_chunk(srv.ws_port, _fields(0, 10), "a.ctb", b"x" * 10, origin="http://127.0.0.1")
    assert status == 200 and js["success"] is True


def test_transfer_encoding_refused(gksdcp_start):
    srv = gksdcp_start()
    boundary, body = build_multipart(_fields(0, 10), "a.ctb", b"x" * 10)
    status, headers, rest = _raw_post(srv, ["Content-Type: multipart/form-data; boundary=" + boundary,
                                            "Transfer-Encoding: chunked"], b"0\r\n\r\n")
    assert status == 400 and b'"success":false' in rest.replace(b" ", b"") and b"-4" in rest
    assert sorted(p.name for p in srv.files.iterdir()) == []


def test_not_multipart_refused(gksdcp_start):
    srv = gksdcp_start()
    status, _, rest = _raw_post(srv, ["Content-Type: text/plain", "Content-Length: 5"], b"hello")
    assert status == 400 and b'"success":false' in rest.replace(b" ", b"")
    status, _, rest = _raw_post(srv, ["Content-Type: multipart/form-data; boundary=B", "Content-Length: 0"], b"")
    assert status == 400 and b'"success":false' in rest.replace(b" ", b"")


def test_fields_after_file_refused(gksdcp_start):
    srv = gksdcp_start()
    b = "BND"
    body = ('--%s\r\nContent-Disposition: form-data; name="Offset"\r\n\r\n0\r\n'
            '--%s\r\nContent-Disposition: form-data; name="Uuid"\r\n\r\nu1\r\n'
            '--%s\r\nContent-Disposition: form-data; name="File"; filename="a.ctb"\r\n\r\nDATA\r\n'
            '--%s\r\nContent-Disposition: form-data; name="TotalSize"\r\n\r\n4\r\n--%s--\r\n' % ((b,) * 5)).encode()
    status, _, rest = _raw_post(srv, ["Content-Type: multipart/form-data; boundary=" + b,
                                      "Content-Length: %d" % len(body)], body)
    assert status == 400 and b'"success":false' in rest.replace(b" ", b"")
    assert sorted(p.name for p in srv.files.iterdir()) == []      # TotalSize unknown at File: nothing created


def test_missing_required_field_refused(gksdcp_start):
    srv = gksdcp_start()
    f = _fields(0, 10)
    del f["Uuid"]
    status, js = upload_chunk(srv.ws_port, f, "a.ctb", b"x" * 10)
    assert status == 400 and _code(js) == -4
    assert sorted(p.name for p in srv.files.iterdir()) == []


def test_total_size_changed_and_overrun(gksdcp_start):
    srv = gksdcp_start()
    assert _send(srv, "a.ctb", b"x" * 100, 0, 300)[1]["success"] is True
    assert _code(_st(_send(srv, "a.ctb", b"x" * 100, 100, 400), 400)) == -4          # TotalSize changed
    assert _code(_st(_send(srv, "a.ctb", b"x" * 250, 100, 300), 400)) == -4          # more data than the total
    p = _parts(srv)
    assert len(p) == 1 and (srv.files / p[0]).stat().st_size == 100            # failed chunk rolled back
    assert _send(srv, "a.ctb", b"x" * 200, 100, 300)[1]["success"] is True
    assert (srv.files / "a.ctb").read_bytes() == b"x" * 300


def test_not_enough_space(gksdcp_start, tmp_path):
    # a small volume (/dev/shm) as the files dir: the 16 MiB reserve plus the upload does not fit
    shm = tempfile.mkdtemp(dir="/dev/shm") if os.path.isdir("/dev/shm") else None
    if shm is None:
        pytest.skip("no /dev/shm")
    try:
        st = os.statvfs(shm)
        total = st.f_bavail * st.f_frsize
        srv = gksdcp_start(("-d", shm))
        status, js = _send(srv, "a.ctb", b"x" * 10, 0, total)       # fits the volume but not volume + reserve
        assert status == 507 and _code(js) == -4
        assert {"field": "common_field", "message": "not enough space"} in js["messages"]
        assert os.listdir(shm) == []
    finally:
        shutil.rmtree(shm, ignore_errors=True)


def test_dropped_connection_chunk_can_be_resent(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    data = os.urandom(2000)
    boundary, body = build_multipart(_fields(0, 2000), "a.ctb", data[:1000])
    s = socket.create_connection(("127.0.0.1", srv.ws_port), 3)
    head = ("POST /uploadFile/upload HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
            "Content-Length: %d\r\n\r\n" % (boundary, len(body))).encode()
    s.sendall(head + body[:len(body) - 400])
    time.sleep(0.3)
    s.close()
    time.sleep(0.4)
    p = _parts(srv)
    assert len(p) == 1 and (srv.files / p[0]).stat().st_size == 0              # kept, rolled back to the chunk start
    assert _send(srv, "a.ctb", data[:1000], 0, 2000)[1]["success"] is True    # same offset again
    assert _send(srv, "a.ctb", data[1000:], 1000, 2000)[1]["success"] is True
    assert (srv.files / "a.ctb").read_bytes() == data
    ws.close()


def test_stale_upload_removed(gksdcp_start):
    srv, ws = _start(gksdcp_start, ("-S", "1"))
    assert _send(srv, "a.ctb", b"x" * 100, 0, 300)[1]["success"] is True
    assert len(_parts(srv)) == 1
    _wait_status(ws, [0], timeout=4.0)
    assert _parts(srv) == []
    assert _code(_st(_send(srv, "a.ctb", b"x" * 100, 100, 300), 400)) == -2          # the session is gone
    ws.close()


def test_stale_timer_runs_from_last_chunk(gksdcp_start):
    srv = gksdcp_start(("-S", "2"))
    assert _send(srv, "a.ctb", b"x" * 100, 0, 300)[1]["success"] is True
    time.sleep(1.4)
    assert _send(srv, "a.ctb", b"x" * 100, 100, 300)[1]["success"] is True
    time.sleep(1.4)                                                            # 2.8 s since the first, 1.4 since the last
    assert _send(srv, "a.ctb", b"x" * 100, 200, 300)[1]["success"] is True


def test_startup_removes_leftover_part_files(tmp_path, gksdcp_start):
    files = tmp_path / "files"
    files.mkdir()
    _touch(files / ".old.ctb.abcd1234.part")
    _touch(files / ".other.part")
    _touch(files / "keep.ctb")
    _touch(files / "x.part")
    _touch(files / ".hidden")
    gksdcp_start()
    assert sorted(p.name for p in files.iterdir()) == [".hidden", "keep.ctb", "x.part"]


def test_unwritable_target_name_never_leaves_files_dir(gksdcp_start, tmp_path):
    srv = gksdcp_start()
    (tmp_path / "escape").mkdir()
    for name in ("../escape/x.ctb", "..\\x.ctb", "sub/../x.ctb"):
        assert _code(_st(_send(srv, name, b"x" * 10, 0, 10), 400)) == -4
    assert list((tmp_path / "escape").iterdir()) == []


def test_one_big_post_streams_without_buffering(gksdcp_start):
    from test_ws import rss_kb
    srv = gksdcp_start()
    before = rss_kb(srv.pid)
    data = os.urandom(24 * CHUNK)
    status, js = upload_chunk(srv.ws_port, _fields(0, len(data)), "big.ctb", data, timeout=30)
    assert status == 200 and js["success"] is True
    assert (srv.files / "big.ctb").read_bytes() == data
    assert rss_kb(srv.pid) - before < 1500            # 24 MiB went through; far less than a chunk stayed


def test_huge_total_size_refused_no_session(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    for total in (2 ** 63 - 1, 2 ** 62, (64 << 30) + 1, 0, -5):
        status, js = _send(srv, "a.ctb", b"x" * 10, 0, total)
        assert status in (400, 507) and _code(js) == -4, (total, status)
    assert _parts(srv) == [] and sorted(p.name for p in srv.files.iterdir()) == []
    ws.close()


def test_long_filename_is_invalid_file_name(gksdcp_start):
    srv = gksdcp_start()
    for name in ("a" * 300 + ".ctb", "a" * 252 + ".ctb", "a" * 197 + ".ctb"):
        status, js = _send(srv, name, b"x" * 10, 0, 10)
        assert status == 400 and {"field": "filename", "message": "invalid file name"} in js["messages"], len(name)


def test_temp_file_is_locked(gksdcp_start):
    import fcntl
    srv = gksdcp_start()
    assert _send(srv, "a.ctb", b"x" * 100, 0, 300)[1]["success"] is True
    part = srv.files / _parts(srv)[0]
    fd = os.open(str(part), os.O_WRONLY)
    with pytest.raises(OSError):
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.close(fd)


def test_half_open_chunk_resent_on_new_connection(gksdcp_start):
    srv = gksdcp_start()
    data = os.urandom(2000)
    boundary, body = build_multipart(_fields(0, 2000), "a.ctb", data[:1000])
    s = socket.create_connection(("127.0.0.1", srv.ws_port), 3)
    head = ("POST /uploadFile/upload HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
            "Content-Length: %d\r\n\r\n" % (boundary, len(body))).encode()
    s.sendall(head + body[:len(body) - 400])          # the rest never comes, the socket stays open
    time.sleep(0.4)
    assert _send(srv, "a.ctb", data[:1000], 0, 2000)[1]["success"] is True
    assert _send(srv, "a.ctb", data[1000:], 1000, 2000)[1]["success"] is True
    assert (srv.files / "a.ctb").read_bytes() == data and _parts(srv) == []
    s.close()


UUID2 = "9a1b2c3d-0d4a-4c55-9a7e-6b1f2d3c4e5f"


def test_new_uuid_takes_over_an_abandoned_upload(gksdcp_start):
    # DragonFruit never sends Cmd 255: a cancelled upload must not block the next one until the stale timeout
    srv = gksdcp_start(("-S", "60"))
    boundary, body = build_multipart(_fields(0, 40000), "old.ctb", b"x" * 40000)
    head = ("POST /uploadFile/upload HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n" % (boundary, len(body))).encode()
    s = socket.create_connection(("127.0.0.1", srv.ws_port), 5)
    s.sendall(head + body[:len(body) // 2])
    time.sleep(0.3)
    s.close()
    time.sleep(0.3)
    assert len(_parts(srv)) == 1
    assert _code(_st(_send(srv, "new.ctb", b"y" * 10, 0, 10, uuid=UUID2), 409)) == -4      # too soon
    time.sleep(5.2)
    assert _send(srv, "new.ctb", b"y" * 10, 0, 10, uuid=UUID2)[1]["success"] is True
    assert (srv.files / "new.ctb").read_bytes() == b"y" * 10
    assert _parts(srv) == []


def test_other_uuid_within_5s_of_a_chunk_is_refused(gksdcp_start):
    srv = gksdcp_start(("-S", "60"))
    assert _send(srv, "a.ctb", b"x" * 100, 0, 300)[1]["success"] is True
    time.sleep(1.0)
    st, js = _send(srv, "b.ctb", b"y" * 10, 0, 10, uuid=UUID2)
    assert st == 409 and {"field": "common_field", "message": "another upload is running"} in js["messages"]
    assert _send(srv, "a.ctb", b"x" * 100, 100, 300)[1]["success"] is True                 # the first upload is intact


def test_replace_refused_while_hook_unreachable(gksdcp_start, mock_hook):
    mock_hook.mode = "absent"                      # the touchscreen can print the old file: gksdcp cannot tell
    srv = gksdcp_start()
    _touch(srv.files / "a.ctb", data=b"old")
    status, js = _send(srv, "a.ctb", b"new" * 10, 0, 30)
    assert status == 409 and {"field": "common_field", "message": "printer state unknown"} in js["messages"]
    assert (srv.files / "a.ctb").read_bytes() == b"old" and _parts(srv) == []
    assert _send(srv, "fresh.ctb", b"n" * 10, 0, 10)[1]["success"] is True    # a new name is allowed


def _abandon(srv, name="old.ctb", total=40000):
    """A first chunk cut off in the middle: the session stays open with no chunk streaming."""
    boundary, body = build_multipart(_fields(0, total), name, b"x" * total)
    head = ("POST /uploadFile/upload HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n" % (boundary, len(body))).encode()
    s = socket.create_connection(("127.0.0.1", srv.ws_port), 5)
    s.sendall(head + body[:len(body) // 2])
    time.sleep(0.3)
    s.close()
    time.sleep(0.3)
    assert len(_parts(srv)) == 1


@pytest.mark.parametrize("cmd", [259, 128, 4096])
def test_abandoned_upload_stops_blocking_after_5s(gksdcp_start, mock_hook, cmd):
    srv, ws = _start(gksdcp_start, ("-S", "60"))
    _touch(srv.files / "keep.ctb")
    data = {259: {"FileList": ["/local/keep.ctb"]}, 128: {"Filename": "/local/keep.ctb"}, 4096: {}}[cmd]
    _abandon(srv)
    _wait_status(ws, [2])
    assert _ack(ws, cmd, data)["Ack"] == 1                                   # within 5 s: the upload can go on
    time.sleep(5.2)
    assert _ack(ws, cmd, data)["Ack"] == 0
    assert _parts(srv) == []
    _wait_status(ws, [0])                                                    # [2] is gone with the session
    ws.close()


def test_takeover_only_for_a_valid_request(gksdcp_start):
    srv = gksdcp_start(("-S", "60"))
    _abandon(srv)
    time.sleep(5.2)
    assert _code(_st(_send(srv, "new.ctb", b"y" * 10, 100, 110, uuid=UUID2), 400)) == -2    # a bad offset
    assert len(_parts(srv)) == 1                                                           # the old upload stays
    assert _send(srv, "old.ctb", b"x" * 40000, 0, 40000)[1]["success"] is True
    assert (srv.files / "old.ctb").read_bytes() == b"x" * 40000


def test_replace_refused_when_print_starts_during_last_chunk(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "a.ctb", data=b"old")
    boundary, body = build_multipart(_fields(0, 3000), "a.ctb", b"n" * 3000)
    head = ("POST /uploadFile/upload HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n" % (boundary, len(body))).encode()
    s = socket.create_connection(("127.0.0.1", srv.ws_port), 5)
    try:
        s.sendall(head + body[:-200])                  # the chunk began while the printer was idle
        time.sleep(0.3)
        mock_hook.state.update(state="printing", file=str(srv.files / "a.ctb"), layer=1, total_layers=10)
        time.sleep(0.7)
        s.sendall(body[-200:])
        status, _, rest = read_http_response(s, 5)
    finally:
        s.close()
    assert status == 409 and b"printer busy" in rest
    assert (srv.files / "a.ctb").read_bytes() == b"old" and _parts(srv) == []
    ws.close()


def test_leftover_part_files_removed_when_storage_appears(gksdcp_start, tmp_path):
    # /media/emmc mounts after gksdcp starts: the cleanup runs once the storage is there
    emmc = tmp_path / "emmc"
    srv = gksdcp_start(("-d", str(emmc)), allow_tmpfs=False)
    assert _code(_st(_send(srv, "a.ctb", b"x" * 10, 0, 10), 503)) == -4
    emmc.mkdir()
    _touch(emmc / ".old.ctb.abcd1234.part")
    _touch(emmc / ".x.ctb.usb.part")
    _touch(emmc / "keep.ctb")
    end = time.monotonic() + 3.0                   # the check runs every second
    while len(list(emmc.iterdir())) > 1 and time.monotonic() < end:
        time.sleep(0.1)
    assert sorted(p.name for p in emmc.iterdir()) == ["keep.ctb"]
