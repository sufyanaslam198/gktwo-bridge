import os
import time

import pytest

from sdcp_client import discover, ws_connect
from test_print import _info, _notice
from test_status import settle


def _ack(ws, cmd, data=None, timeout=2.5):
    return ws.wait_response(ws.request(cmd, data), timeout=timeout)["Data"]["Data"]


def _start(gksdcp_start, extra=()):
    srv = gksdcp_start(extra)
    ws = ws_connect(srv.ws_port)
    settle(ws)
    return srv, ws


def _touch(path, mtime=None, data=b"x"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    if mtime is not None:
        os.utime(path, (mtime, mtime))


def _mounts(tmp_path, usb):
    m = tmp_path / "mounts"
    m.write_text("proc /proc proc rw 0 0\n/dev/sda1 %s vfat rw 0 0\n" % usb)
    return m


def test_list_local_order_and_filter(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    now = int(time.time())
    (srv.files / "oldfolder").mkdir()
    (srv.files / "newfolder").mkdir()
    os.utime(srv.files / "oldfolder", (now - 500, now - 500))
    os.utime(srv.files / "newfolder", (now - 5, now - 5))
    _touch(srv.files / "old.ctb", now - 300)
    _touch(srv.files / "new.CTB", now - 10)
    _touch(srv.files / "mid.ctb", now - 100)
    _touch(srv.files / "x.ctb.part", now)
    _touch(srv.files / ".hidden.ctb", now)
    _touch(srv.files / "note.txt", now)
    _touch(srv.files / "a:b.ctb", now)                   # name the protocol cannot carry
    (srv.files / "link.ctb").symlink_to(srv.files / "old.ctb")
    d = _ack(ws, 258, {"Url": "/local/"})
    assert d["Ack"] == 0
    assert [(e["name"], e["type"]) for e in d["FileList"]] == [
        ("/local/newfolder", 0), ("/local/oldfolder", 0),
        ("/local/new.CTB", 1), ("/local/mid.ctb", 1), ("/local/old.ctb", 1)]
    e = d["FileList"][2]
    assert e["storageType"] == 0 and e["totalSize"] > 0 and 0 <= e["usedSize"] <= e["totalSize"]
    assert set(e) == {"name", "usedSize", "totalSize", "storageType", "type"}
    assert _ack(ws, 258) == d                            # Url defaults to /local/
    assert _ack(ws, 258, {"Url": "/local"}) == d
    ws.close()


def test_list_subfolder_and_invalid(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "sub" / "p.ctb")
    d = _ack(ws, 258, {"Url": "/local/sub/"})
    assert d["Ack"] == 0 and [e["name"] for e in d["FileList"]] == ["/local/sub/p.ctb"]
    assert _ack(ws, 258, {"Url": "sub"})["FileList"][0]["name"] == "/local/sub/p.ctb"
    for url in ("/local/nope/", "/local/../", "/other/", "/local/sub/p.ctb", 5):
        d = _ack(ws, 258, {"Url": url})
        assert d["Ack"] == 1 and d["FileList"] == [], url
    ws.close()


def test_list_usb_unmounted_and_mounted(gksdcp_start, tmp_path):
    srv, ws = _start(gksdcp_start)
    _touch(srv.usb / "u.ctb")
    d = _ack(ws, 258, {"Url": "/usb/"})
    assert d["Ack"] == 0 and d["FileList"] == []         # not mounted
    ws.close()
    srv, ws = _start(gksdcp_start, ["-M", str(_mounts(tmp_path, srv.usb))])
    d = _ack(ws, 258, {"Url": "/usb/"})
    assert d["Ack"] == 0 and [(e["name"], e["storageType"]) for e in d["FileList"]] == [("/usb/u.ctb", 1)]
    ws.close()


def test_list_usb_mount_appears_later(gksdcp_start, tmp_path):
    m = tmp_path / "mounts"
    m.write_text("")
    srv, ws = _start(gksdcp_start, ["-M", str(m)])
    _touch(srv.usb / "u.ctb")
    assert _ack(ws, 258, {"Url": "/usb/"})["FileList"] == []
    _mounts(tmp_path, srv.usb)
    assert len(_ack(ws, 258, {"Url": "/usb/"})["FileList"]) == 1
    ws.close()


def test_list_capped_at_500_keeps_newest(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    now = int(time.time())
    for i in range(520):
        _touch(srv.files / ("f%03d.ctb" % i), now - 1000 + i, data=b"")      # ascending mtimes
    d = _ack(ws, 258, {"Url": "/local/"}, timeout=5)
    names = [e["name"] for e in d["FileList"]]
    assert d["Ack"] == 0 and len(names) == 500
    assert names[0] == "/local/f519.ctb" and names[-1] == "/local/f020.ctb"   # newest 500, newest first
    ws.close()


def test_delete_file_and_folder(gksdcp_start, tmp_path):
    srv, ws = _start(gksdcp_start)
    outside = tmp_path / "outside"
    _touch(outside / "keep.txt")
    _touch(srv.files / "a.ctb")
    _touch(srv.files / "keep.ctb")
    _touch(srv.files / "dir" / "sub" / "deep" / "b.ctb")
    (srv.files / "dir" / "link").symlink_to(outside)     # must be unlinked, never followed
    d = _ack(ws, 259, {"FileList": ["/local/a.ctb"], "FolderList": ["/local/dir/"]})
    assert d["Ack"] == 0 and "ErrData" not in d
    assert not (srv.files / "a.ctb").exists() and not (srv.files / "dir").exists()
    assert (srv.files / "keep.ctb").exists() and (outside / "keep.txt").exists()
    ws.close()


def test_delete_not_through_symlinked_folder(gksdcp_start, tmp_path):
    srv, ws = _start(gksdcp_start)
    outside = tmp_path / "outside"
    _touch(outside / "keep.ctb")
    (srv.files / "ln").symlink_to(outside)
    d = _ack(ws, 259, {"FileList": ["/local/ln/keep.ctb"]})
    assert d["ErrData"] == ["/local/ln/keep.ctb"] and (outside / "keep.ctb").exists()
    ws.close()


def test_delete_errors_in_errdata(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "ok.ctb")
    _touch(srv.files / "dir" / "x.ctb")
    (srv.files / "ln.ctb").symlink_to(srv.files / "ok.ctb")
    d = _ack(ws, 259, {
        "FileList": ["/local/missing.ctb", "/local/ok.ctb", "/local/dir", "/local/ln.ctb", "/local/../x", "/local/"],
        "FolderList": ["/local/", "/local/ok2", "/local/dir/x.ctb"]})
    assert d["Ack"] == 0
    assert d["ErrData"] == ["/local/missing.ctb", "/local/dir", "/local/ln.ctb", "/local/../x", "/local/",
                            "/local/", "/local/ok2", "/local/dir/x.ctb"]
    assert not (srv.files / "ok.ctb").exists() and (srv.files / "dir" / "x.ctb").exists()
    assert (srv.files / "ln.ctb").is_symlink()
    ws.close()


def test_delete_long_list_gets_a_response(gksdcp_start):
    # more entries than the 512-token fast path holds (the file list itself can have 500)
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "keep.ctb")
    _touch(srv.files / "f599.ctb")
    paths = ["/local/f%03d.ctb" % i for i in range(600)]
    d = _ack(ws, 259, {"FileList": paths}, timeout=5.0)
    assert d["Ack"] == 0 and len(d["ErrData"]) == 599 and "/local/f599.ctb" not in d["ErrData"]
    assert (srv.files / "keep.ctb").exists() and not (srv.files / "f599.ctb").exists()
    ws.close()


@pytest.mark.parametrize("cmd,key", [(259, "FileList"), (321, "Id")])
def test_largest_array_request_does_not_stall_the_loop(gksdcp_start, cmd, key):
    # a 64 KB frame of the smallest elements: the loop must answer it, and a status request behind it, at once
    srv, ws = _start(gksdcp_start)
    n = (65536 - 400) // 3                           # "0, " per element
    t0 = time.monotonic()
    big = ws.request(cmd, {key: [0] * n})
    small = ws.request(0)
    ws.wait_response(big, timeout=10.0)
    ws.wait_response(small, timeout=10.0)
    assert time.monotonic() - t0 < 0.05                # quadratic array walks take 0.15 s on x86 here, seconds on ARM
    ws.close()


def test_delete_depth_limit(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    ok = srv.files / "d0" / "1" / "2" / "3" / "4" / "5" / "6" / "7" / "8"      # 8 levels below d0
    _touch(ok / "f.ctb")
    too_deep = srv.files / "e0" / "1" / "2" / "3" / "4" / "5" / "6" / "7" / "8" / "9"
    _touch(too_deep / "f.ctb")
    d = _ack(ws, 259, {"FolderList": ["/local/d0", "/local/e0"]})
    assert d["ErrData"] == ["/local/e0"]
    assert not (srv.files / "d0").exists() and (srv.files / "e0").exists()
    ws.close()


def test_delete_bad_request(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 259, {})["Ack"] == 1
    assert _ack(ws, 259, {"FileList": "a"})["Ack"] == 1
    ws.close()


def test_delete_refused_while_printing(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "a.ctb")
    for state in ("printing", "paused", "starting"):
        mock_hook.state.update(state=state, file=str(srv.files / "job.ctb"), layer=1, total_layers=10, phase="exposing")
        time.sleep(0.6)
        ws.msgs.clear()
        d = _ack(ws, 259, {"FileList": ["/local/a.ctb"]})
        assert d["Ack"] == 1, state
        assert (srv.files / "a.ctb").exists()
        assert _notice(ws) == "printer busy"
    mock_hook.state.update(state="complete")
    time.sleep(0.6)
    assert _ack(ws, 259, {"FileList": ["/local/a.ctb"]})["Ack"] == 0
    assert not (srv.files / "a.ctb").exists()
    ws.close()


def test_delete_refused_while_hook_unreachable(gksdcp_start, mock_hook):
    mock_hook.mode = "absent"                      # gksdcp cannot tell whether the touchscreen prints
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "a.ctb")
    assert _ack(ws, 259, {"FileList": ["/local/a.ctb"]})["Ack"] == 1
    assert _notice(ws) == "printer state unknown"
    assert (srv.files / "a.ctb").exists()
    ws.close()


def test_delete_current_print_file_refused(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "dir" / "job.ctb")
    _touch(srv.files / "dir" / "other.ctb")
    _touch(srv.files / "free" / "o.ctb")
    mock_hook.state.update(state="complete", file=str(srv.files / "dir" / "job.ctb"))
    time.sleep(0.6)
    d = _ack(ws, 259, {"FileList": ["/local/dir/job.ctb"], "FolderList": ["/local/dir", "/local/free"]})
    assert d["Ack"] == 0 and d["ErrData"] == ["/local/dir/job.ctb", "/local/dir"]
    assert (srv.files / "dir" / "job.ctb").exists() and (srv.files / "dir" / "other.ctb").exists()
    assert not (srv.files / "free").exists()
    ws.close()


def test_delete_usb_unmounted_is_error(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    _touch(srv.usb / "u.ctb")
    d = _ack(ws, 259, {"FileList": ["/usb/u.ctb"]})
    assert d["Ack"] == 0 and d["ErrData"] == ["/usb/u.ctb"] and (srv.usb / "u.ctb").exists()
    ws.close()


def test_delete_usb_mounted(gksdcp_start, tmp_path):
    srv, ws = _start(gksdcp_start)
    ws.close()
    srv, ws = _start(gksdcp_start, ["-M", str(_mounts(tmp_path, srv.usb))])
    _touch(srv.usb / "u.ctb")
    d = _ack(ws, 259, {"FileList": ["/usb/u.ctb"]})
    assert d["Ack"] == 0 and "ErrData" not in d and not (srv.usb / "u.ctb").exists()
    ws.close()


def test_rename_persists_and_pushes(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 192, {"Name": "Resin Bot é"})["Ack"] == 0
    assert ws.wait_topic("attributes")["Attributes"]["Name"] == "Resin Bot é"
    ws.close()
    srv.proc.terminate()
    srv.proc.wait(5)
    srv2 = gksdcp_start()
    assert discover(srv2.udp_port)["Data"]["Name"] == "Resin Bot é"


@pytest.mark.parametrize("data", [{}, {"Name": ""}, {"Name": 5}, {"Name": "x" * 65}, {"Name": "a\u0001"}])
def test_rename_invalid(gksdcp_start, data):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 192, data)["Ack"] == 1
    ws.close()
    assert discover(srv.udp_port)["Data"]["Name"] == "GKtwo"


def test_start_subfolder_file_ack2(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "sub" / "a.ctb")
    assert _ack(ws, 128, {"Filename": "/local/sub/a.ctb"})["Ack"] == 2
    assert _notice(ws) == "move the file to the top folder to print it"
    assert [r for r in mock_hook.received if r.get("cmd") == "start"] == []
    ws.close()


def test_delete_refused_right_after_start(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    _touch(srv.files / "a.ctb")
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 0
    d = _ack(ws, 259, {"FileList": ["/local/a.ctb"]})          # snapshot still idle, start not yet visible
    assert d["Ack"] == 1 and (srv.files / "a.ctb").exists()
    assert _notice(ws) == "printer busy"
    ws.close()


@pytest.mark.parametrize("inside", [True, False])
def test_delete_symlinked_folder_refused(gksdcp_start, tmp_path, inside):
    srv, ws = _start(gksdcp_start)
    target = (srv.files / "real") if inside else (tmp_path / "outside")
    _touch(target / "keep.ctb")
    (srv.files / "ln").symlink_to(target)
    d = _ack(ws, 259, {"FolderList": ["/local/ln"]})
    assert d["Ack"] == 0 and d["ErrData"] == ["/local/ln"]
    assert (srv.files / "ln").is_symlink() and (target / "keep.ctb").exists()
    ws.close()
