import json
import os
import shutil
import socket
import struct
import tempfile
import time

import pytest

from sdcp_client import upload_chunk, ws_connect
from test_files import _ack
from test_print import _hook_cmds, _notice, _notices
from test_status import settle

MAGIC = struct.pack("<I", 0x12FD0106)
UUID = "3f2b8c1e-0d4a-4c55-9a7e-6b1f2d3c4e5f"


def _ctb(path, size=1024, magic=MAGIC):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(magic + os.urandom(size - 4))
    return path.read_bytes()


def _boot(gksdcp_start, tmp_path, extra=(), mounted=True, allow_tmpfs=True):
    """Server whose mounts file lists the usb dir (the dir is tmp_path/usb, as in conftest)."""
    usb = tmp_path / "usb"
    usb.mkdir(exist_ok=True)
    m = tmp_path / "mounts"
    m.write_text("proc /proc proc rw 0 0\n" + ("/dev/sda1 %s vfat rw 0 0\n" % usb if mounted else ""))
    srv = gksdcp_start(["-M", str(m)] + list(extra), allow_tmpfs=allow_tmpfs)
    ws = ws_connect(srv.ws_port)
    settle(ws)
    return srv, ws, m


def _wait_attr(ws, want, timeout=4.0):
    end = time.monotonic() + timeout
    while True:
        a = ws.wait_topic("attributes", max(0.05, end - time.monotonic()))
        if a["Attributes"]["UsbDiskStatus"] == want:
            return a


def _wait_cur(ws, pred, timeout=5.0):
    end = time.monotonic() + timeout
    while True:
        s = ws.wait_topic("status", max(0.05, end - time.monotonic()))["Status"]
        if pred(s):
            return s


def _wait_start(mock_hook, timeout=8.0):
    end = time.monotonic() + timeout
    while not _hook_cmds(mock_hook, "start") and time.monotonic() < end:
        time.sleep(0.05)


def _slow(monkeypatch, ms=40):
    monkeypatch.setenv("GKSDCP_TEST_USB_STEP_MS", str(ms))     # debug builds only: pause between copy steps


def test_usb_status_attribute_changes(gksdcp_start, tmp_path):
    srv, ws, m = _boot(gksdcp_start, tmp_path, mounted=False)
    ws.request(1)
    assert ws.wait_topic("attributes")["Attributes"]["UsbDiskStatus"] == 0
    settle(ws, 0.3)
    m.write_text("proc /proc proc rw 0 0\n/dev/sda1 %s vfat rw 0 0\n" % srv.usb)
    _wait_attr(ws, 1)
    m.write_text("proc /proc proc rw 0 0\n")
    _wait_attr(ws, 0)
    ws.close()


def test_print_from_usb_copies_then_starts(gksdcp_start, tmp_path, mock_hook):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    data = _ctb(srv.usb / "x.ctb", 20 * 1024 * 1024)
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 0
    s = _wait_cur(ws, lambda s: s["CurrentStatus"] == [1] and s["PrintInfo"]["Status"] == 10)
    assert s["PrintInfo"]["TaskId"] != ""
    _wait_start(mock_hook)
    assert _hook_cmds(mock_hook, "start") == [{"cmd": "start", "path": "%s/x.ctb" % srv.files}]
    assert (srv.files / "x.ctb").read_bytes() == data
    assert sorted(p.name for p in srv.files.iterdir()) == ["x.ctb"]
    ws.close()


def test_usb_copy_cancel_by_stop(gksdcp_start, tmp_path, mock_hook, monkeypatch):
    _slow(monkeypatch)
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb", 20 * 1024 * 1024)
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 0
    time.sleep(0.3)
    assert list(srv.files.iterdir()), "copy should be under way"
    assert _ack(ws, 130)["Ack"] == 0
    time.sleep(0.5)
    assert list(srv.files.iterdir()) == []
    assert _hook_cmds(mock_hook, "start") == [] and _hook_cmds(mock_hook, "stop") == []
    s = _wait_cur(ws, lambda s: s["CurrentStatus"] == [0])
    assert s["PrintInfo"]["Status"] == 8
    ids = _ack(ws, 320)["HistoryData"]
    assert len(ids) == 1
    d = _ack(ws, 321, {"Id": ids})["HistoryDetailList"]
    assert d[0]["TaskStatus"] == 3
    ws.close()


def test_usb_not_ctb_ack6(gksdcp_start, tmp_path, mock_hook):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "bad.ctb", 1000, magic=b"\x00\x01\x02\x03")
    (srv.usb / "note.txt").write_bytes(MAGIC + b"x" * 100)
    (srv.usb / "dir.ctb").mkdir()
    for name in ("bad.ctb", "note.txt", "dir.ctb"):
        assert _ack(ws, 128, {"Filename": "/usb/" + name})["Ack"] == 6
    assert _ack(ws, 128, {"Filename": "/usb/none.ctb"})["Ack"] == 2
    assert _hook_cmds(mock_hook, "start") == [] and list(srv.files.iterdir()) == []
    assert _ack(ws, 320)["HistoryData"] == []
    ws.close()


def test_usb_unmounted_ack2(gksdcp_start, tmp_path):
    srv, ws, _ = _boot(gksdcp_start, tmp_path, mounted=False)
    _ctb(srv.usb / "x.ctb")
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 2
    ws.close()


def test_usb_no_space_ack4(gksdcp_start, tmp_path, mock_hook):
    srv, ws, _ = _boot(gksdcp_start, tmp_path, extra=["-R", str(1 << 60)])
    _ctb(srv.usb / "x.ctb")
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 4
    assert _notice(ws) == "not enough space on internal storage"
    assert list(srv.files.iterdir()) == [] and _ack(ws, 320)["HistoryData"] == []
    ws.close()


@pytest.mark.parametrize("state", ["printing", "paused", "starting"])
def test_usb_refused_unless_idle(gksdcp_start, tmp_path, mock_hook, state):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb")
    mock_hook.state.update(state=state, file="%s/other.ctb" % srv.files)
    time.sleep(0.7)
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 1
    assert list(srv.files.iterdir()) == []
    ws.close()


def _in_order(ws, rid, quiet=1.5):
    """The response to rid and the notices, in the order they arrive."""
    seen = []
    end = time.monotonic() + quiet
    while time.monotonic() < end:
        try:
            op, payload = ws.recv(max(0.05, end - time.monotonic()))
        except socket.timeout:
            break
        if op != 1:
            continue
        try:
            m = json.loads(payload)
        except ValueError:
            continue
        topic, d = m.get("Topic", ""), m.get("Data", {})
        if topic.startswith("sdcp/response/") and d.get("RequestID") == rid:
            seen.append(("response", d["Data"]["Ack"]))
        elif topic.startswith("sdcp/notice/"):
            seen.append(("notice", d["Data"]["Message"]))
    return seen


def test_usb_copy_refusal_answers_before_its_notice(gksdcp_start, tmp_path, mock_hook):
    # a client (the web page) matches a refusal reason to the request only after it has the response
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb")
    (srv.files / ".x.ctb.usb.part").mkdir()          # the copy cannot open its temporary file
    rid = ws.request(128, {"Filename": "/usb/x.ctb"})
    assert _in_order(ws, rid) == [("response", 4), ("notice", "cannot write to internal storage")]
    ws.close()


def test_usb_refused_while_hook_unreachable(gksdcp_start, tmp_path, mock_hook):
    mock_hook.mode = "absent"
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb")
    (srv.files / "x.ctb").write_bytes(b"old")
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 1
    assert _notice(ws) == "printer state unknown"
    assert (srv.files / "x.ctb").read_bytes() == b"old" and _ack(ws, 320)["HistoryData"] == []
    ws.close()


def test_usb_refused_while_storage_not_mounted(gksdcp_start, tmp_path, mock_hook):
    # before the eMMC mounts, the files folder is on tmpfs: a copy there is lost at the next restart
    if not os.path.isdir("/dev/shm"):
        pytest.skip("no /dev/shm")
    shm = tempfile.mkdtemp(dir="/dev/shm")
    try:
        srv, ws, _ = _boot(gksdcp_start, tmp_path, extra=["-d", shm, "-R", "0"], allow_tmpfs=False)
        _ctb(srv.usb / "x.ctb")
        assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 1
        assert _notice(ws) == "storage not mounted"
        assert os.listdir(shm) == [] and _ack(ws, 320)["HistoryData"] == []
        ws.close()
    finally:
        shutil.rmtree(shm, ignore_errors=True)


def test_usb_dest_is_current_print_file_refused(gksdcp_start, tmp_path, mock_hook):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb")
    (srv.files / "x.ctb").write_bytes(b"old")
    mock_hook.state.update(state="error", file="%s/x.ctb" % srv.files)
    time.sleep(0.7)
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 1
    assert (srv.files / "x.ctb").read_bytes() == b"old"
    ws.close()


def test_usb_overwrites_existing_when_idle(gksdcp_start, tmp_path, mock_hook):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    data = _ctb(srv.usb / "sub" / "x.ctb")
    (srv.files / "x.ctb").write_bytes(b"old")
    assert _ack(ws, 128, {"Filename": "/usb/sub/x.ctb"})["Ack"] == 0
    _wait_start(mock_hook)
    assert (srv.files / "x.ctb").read_bytes() == data
    ws.close()


def test_usb_busy_guards_during_copy(gksdcp_start, tmp_path, mock_hook, monkeypatch):
    _slow(monkeypatch, 60)
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb", 20 * 1024 * 1024)
    (srv.files / "keep.ctb").write_bytes(b"k")
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 0
    time.sleep(0.2)
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 1
    assert _ack(ws, 259, {"FileList": ["/local/keep.ctb"]})["Ack"] == 1
    assert (srv.files / "keep.ctb").exists()
    status, js = upload_chunk(srv.ws_port, {"S-File-MD5": "", "Check": "0", "Offset": 0, "Uuid": UUID,
                                           "TotalSize": 10}, "new.ctb", b"x" * 10)
    assert status == 409 and {"field": "common_field", "message": "printer busy"} in js["messages"]
    assert _ack(ws, 130)["Ack"] == 0
    ws.close()


def test_usb_print_refused_during_upload(gksdcp_start, tmp_path):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb")
    res = upload_chunk(srv.ws_port, {"S-File-MD5": "", "Check": "0", "Offset": 0, "Uuid": UUID, "TotalSize": 200},
                       "a.ctb", b"x" * 100)
    assert res[1]["success"] is True
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 1
    ws.close()


def test_usb_pulled_mid_copy_fails_cleanly(gksdcp_start, tmp_path, mock_hook, monkeypatch):
    _slow(monkeypatch)
    srv, ws, m = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb", 20 * 1024 * 1024)
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 0
    time.sleep(0.3)
    m.write_text("proc /proc proc rw 0 0\n")
    assert _notice(ws) == "USB drive removed during copy"
    s = _wait_cur(ws, lambda s: s["CurrentStatus"] == [0])
    assert s["PrintInfo"]["Status"] == 8
    assert list(srv.files.iterdir()) == [] and _hook_cmds(mock_hook, "start") == []
    d = _ack(ws, 321, {"Id": _ack(ws, 320)["HistoryData"]})["HistoryDetailList"]
    assert d[0]["TaskStatus"] == 3
    ws.close()


def test_usb_source_shrinks_mid_copy(gksdcp_start, tmp_path, mock_hook, monkeypatch):
    _slow(monkeypatch)
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    f = srv.usb / "x.ctb"
    _ctb(f, 20 * 1024 * 1024)
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 0
    time.sleep(0.3)
    os.truncate(f, 1024 * 1024)                          # a short read: the drive lost the rest
    _notice(ws)
    time.sleep(0.3)
    assert list(srv.files.iterdir()) == [] and _hook_cmds(mock_hook, "start") == []
    ws.close()


def test_usb_start_error_closes_task_3(gksdcp_start, tmp_path, mock_hook):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb")
    mock_hook.errors["start"] = "file resolution does not match printer"
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 0
    assert _notice(ws) == "file resolution does not match printer"
    s = _wait_cur(ws, lambda s: s["CurrentStatus"] == [0])
    assert s["PrintInfo"]["Status"] == 8
    d = _ack(ws, 321, {"Id": _ack(ws, 320)["HistoryData"]})["HistoryDetailList"]
    assert len(d) == 1 and d[0]["TaskStatus"] == 3
    ws.close()


def test_cmd0_answered_during_big_copy(gksdcp_start, tmp_path, mock_hook):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb", 20 * 1024 * 1024)
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 0
    worst = 0.0
    for _ in range(8):
        t = time.monotonic()
        ws.wait_response(ws.request(0), timeout=2.0)
        worst = max(worst, time.monotonic() - t)
    assert worst < 0.5
    ws.close()


def test_startup_removes_leftover_usb_part(gksdcp_start, tmp_path):
    (tmp_path / "files").mkdir()
    (tmp_path / "files" / ".x.ctb.usb.part").write_bytes(b"junk")
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    assert list(srv.files.iterdir()) == []
    ws.close()


def test_touchscreen_print_during_copy_takes_over_the_task(gksdcp_start, tmp_path, mock_hook, monkeypatch):
    _slow(monkeypatch)
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb", 20 * 1024 * 1024)
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 0
    time.sleep(0.3)
    mock_hook.state.update(state="printing", file="%s/other.ctb" % srv.files, layer=1, total_layers=10,
                           phase="exposing")
    notices = _notices(ws, 4.5)                          # the copy finishes meanwhile
    assert notices == []
    assert _hook_cmds(mock_hook, "start") == []
    assert list(srv.files.iterdir()) == []               # the copy was ours and is removed
    ids = _ack(ws, 320)["HistoryData"]
    assert len(ids) == 1
    t = _ack(ws, 321, {"Id": ids})["HistoryDetailList"][0]
    assert t["EndTime"] == 0 and t["TaskStatus"] == 0 and t["TaskName"] == "other.ctb"
    ws.request(0)
    s = _wait_cur(ws, lambda s: s["CurrentStatus"] == [1])
    assert s["PrintInfo"]["TaskId"] == ids[0] and s["PrintInfo"]["Filename"] == "/local/other.ctb"
    mock_hook.state.update(state="idle", file=None)
    _wait_cur(ws, lambda s: s["CurrentStatus"] == [0])
    t = _ack(ws, 321, {"Id": ids})["HistoryDetailList"][0]
    assert t["TaskStatus"] == 3 and t["EndTime"] > 0
    ws.close()


def test_usb_start_refused_removes_new_copy_keeps_replaced(gksdcp_start, tmp_path, mock_hook):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    data = _ctb(srv.usb / "x.ctb")
    _ctb(srv.usb / "y.ctb")
    (srv.files / "y.ctb").write_bytes(b"old")
    mock_hook.errors["start"] = "file not found"
    for name in ("x.ctb", "y.ctb"):
        assert _ack(ws, 128, {"Filename": "/usb/" + name})["Ack"] == 0
        assert _notice(ws) == "file not found"
        time.sleep(1.2)                                  # past the start guard
    assert not (srv.files / "x.ctb").exists()
    assert (srv.files / "y.ctb").exists() and (srv.files / "y.ctb").read_bytes() != b"old"
    assert data != b""
    ws.close()


def test_usb_stop_between_copy_and_start_reply(gksdcp_start, tmp_path, mock_hook):
    srv, ws, _ = _boot(gksdcp_start, tmp_path)
    _ctb(srv.usb / "x.ctb", 2 * 1024 * 1024)
    mock_hook.delays["start"] = 1.0
    assert _ack(ws, 128, {"Filename": "/usb/x.ctb"})["Ack"] == 0
    end = time.monotonic() + 5
    while not _hook_cmds(mock_hook, "start") and time.monotonic() < end:
        time.sleep(0.02)
    assert _ack(ws, 130)["Ack"] == 0
    assert _hook_cmds(mock_hook, "stop") == []           # not before the start has been answered
    end = time.monotonic() + 4
    while not _hook_cmds(mock_hook, "stop") and time.monotonic() < end:
        time.sleep(0.05)
    assert [c["cmd"] for c in mock_hook.received if c["cmd"] in ("start", "stop")] == ["start", "stop"]
    ws.close()
