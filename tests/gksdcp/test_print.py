import time

import pytest

from sdcp_client import ws_connect
from test_status import settle


def _ack(ws, cmd, data=None, timeout=2.5):
    r = ws.wait_response(ws.request(cmd, data), timeout=timeout)
    return r["Data"]["Data"]


def _notice(ws):
    return ws.wait_topic("notice", timeout=2.0)["Data"]["Data"]["Message"]


def _hook_cmds(mock_hook, cmd):
    return [r for r in mock_hook.received if r.get("cmd") == cmd]


def _start(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    return srv, ws


def _info(ws, pred, timeout=3.0):
    end = time.monotonic() + timeout
    while True:
        st = ws.wait_topic("status", timeout=max(0.05, end - time.monotonic()))["Status"]["PrintInfo"]
        if pred(st):
            return st


def _notices(ws, quiet=1.0):
    """Drain the socket for a while; return the notice messages seen."""
    end = time.monotonic() + quiet
    while time.monotonic() < end:
        try:
            ws.wait_topic("status", timeout=max(0.05, end - time.monotonic()))
        except Exception:
            pass
    return [m["Data"]["Data"]["Message"] for m in ws.msgs if m.get("Topic", "").startswith("sdcp/notice/")]


def test_start_local_maps_path(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    for name in ("/local/a.ctb", "a.ctb"):
        mock_hook.received.clear()
        assert _ack(ws, 128, {"Filename": name, "StartLayer": 0})["Ack"] == 0
        assert _hook_cmds(mock_hook, "start") == [{"cmd": "start", "path": "%s/a.ctb" % srv.files}]
        mock_hook.state.update(state="starting", file="%s/a.ctb" % srv.files)
        assert _info(ws, lambda st: st["TaskId"] != "")["Filename"] == "/local/a.ctb"   # canonical, even for a bare name
        mock_hook.state.update(state="idle", file=None)
        time.sleep(1.2)            # past the bridge's own start guard, so the next start is accepted
    ws.close()


def test_start_unicode_name(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": "/local/Drache_Ü (2).ctb"})["Ack"] == 0
    assert _hook_cmds(mock_hook, "start")[0]["path"] == "%s/Drache_Ü (2).ctb" % srv.files
    ws.close()


@pytest.mark.parametrize("name", ['q"x.ctb', "a\\b.ctb", "a:b.ctb", ".hidden.ctb"])
def test_start_unrepresentable_name_ack2(gksdcp_start, mock_hook, name):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": name})["Ack"] == 2
    assert _hook_cmds(mock_hook, "start") == []
    ws.close()


@pytest.mark.parametrize("name", ["/local/sub/a.ctb", "/usb/a.ctb", "sub/a.ctb", "/local/", "", "/local/..", ".."])
def test_start_bad_path_ack2(gksdcp_start, mock_hook, name):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": name})["Ack"] == 2
    assert _hook_cmds(mock_hook, "start") == []
    ws.close()


def test_start_filename_missing_or_not_string(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {})["Ack"] == 2
    assert _notice(ws) == "bad file path"
    assert _ack(ws, 128, {"Filename": 5})["Ack"] == 2
    assert _notice(ws) == "bad file path"
    assert _hook_cmds(mock_hook, "start") == []
    ws.close()


ACK_TABLE = [
    ("printer busy", 1), ("hook starting", 1), ("dismiss the finished print on the printer first", 1),
    ("Z is moving", 1), ("printer not responding", 1), ("something new", 1),
    ("file not found", 2), ("file is not in the files folder", 2), ("bad file path", 2),
    ("cannot read file", 4), ("cannot open file", 4), ("cannot start file check", 4),
    ("file resolution does not match printer", 5),
    ("not a ctb file", 6), ("unsupported file format", 6), ("unsupported ctb version", 6),
    ("only .ctb files can be printed", 6),
]


@pytest.mark.parametrize("err,ack", ACK_TABLE)
def test_start_ack_mapping(gksdcp_start, mock_hook, err, ack):
    srv, ws = _start(gksdcp_start)
    mock_hook.errors["start"] = err
    d = _ack(ws, 128, {"Filename": "/local/a.ctb", "StartLayer": 0})
    assert d["Ack"] == ack
    assert _notice(ws) == err
    ws.close()


def test_start_refused_aborts_history(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    mock_hook.errors["start"] = "file not found"
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 2
    assert _ack(ws, 320)["HistoryData"] == []
    ws.close()


def test_start_layer_refused(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": "/local/a.ctb", "StartLayer": 5})["Ack"] == 1
    assert _notice(ws) == "start layer is not supported"
    assert _hook_cmds(mock_hook, "start") == []
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 0      # absent is fine
    ws.close()


def test_start_refused_while_print_live(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    mock_hook.state.update(state="printing", file="/media/emmc/job.ctb", layer=1, total_layers=10, phase="exposing")
    _info(ws, lambda st: st["TaskId"] != "")
    mock_hook.received.clear()
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 1
    assert _notice(ws) == "printer busy"
    assert _hook_cmds(mock_hook, "start") == []
    ws.close()


def test_second_start_while_first_pending_refused(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 0
    assert _ack(ws, 128, {"Filename": "/local/b.ctb"})["Ack"] == 1       # snapshot still idle, but a start is pending
    assert len(_hook_cmds(mock_hook, "start")) == 1
    ws.close()


def test_start_creates_one_task(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 0
    mock_hook.state.update(state="starting", file="%s/a.ctb" % srv.files)
    time.sleep(0.9)
    mock_hook.state.update(state="printing", layer=1, total_layers=10, phase="exposing")
    time.sleep(0.9)
    assert len(_ack(ws, 320)["HistoryData"]) == 1
    ws.close()


def test_start_hook_hung_answers_in_time(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    mock_hook.mode = "hang"
    t0 = time.monotonic()
    d = _ack(ws, 128, {"Filename": "/local/a.ctb"}, timeout=2.5)
    assert d["Ack"] == 1
    assert time.monotonic() - t0 < 2.5
    assert _notice(ws) == "printer not responding"
    assert _ack(ws, 320, timeout=1.0)["HistoryData"] == []        # the aborted task is gone
    ws.close()


def test_start_dragonfruit_flow(gksdcp_start, mock_hook):
    # list /local/, pick the entry, send Cmd 128 with that exact string
    srv, ws = _start(gksdcp_start)
    (srv.files / "Drache_Ü (2).ctb").write_bytes(b"x")
    files = _ack(ws, 258, {"Url": "/local/"})["FileList"]
    assert [e["name"] for e in files] == ["/local/Drache_Ü (2).ctb"]
    assert _ack(ws, 128, {"Filename": files[0]["name"], "StartLayer": 0})["Ack"] == 0
    assert _hook_cmds(mock_hook, "start") == [{"cmd": "start", "path": "%s/Drache_Ü (2).ctb" % srv.files}]
    mock_hook.state.update(state="starting", file="%s/Drache_Ü (2).ctb" % srv.files)
    st = _info(ws, lambda st: st["TaskId"] != "")
    assert st["Filename"] == "/local/Drache_Ü (2).ctb"
    ws.close()


def test_start_failure_sets_error_number(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 0
    mock_hook.state.update(state="starting", file="%s/a.ctb" % srv.files)
    time.sleep(0.9)
    mock_hook.state.update(state="idle", file=None, message="file check failed")
    st = _info(ws, lambda st: st["ErrorNumber"] == 2)
    assert st["Status"] == 8        # latched by status_track on starting -> idle, in the same frame
    # one notice, however many snapshots repeat the message
    assert _notices(ws, 1.5) == ["file check failed"]
    ws.close()


@pytest.mark.parametrize("msg,en", [("cannot open file", 2), ("cannot read file", 2), ("not a ctb file", 4),
                                    ("start cancelled", 0)])
def test_start_failure_error_numbers(gksdcp_start, mock_hook, msg, en):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 0
    mock_hook.state.update(state="starting", file="%s/a.ctb" % srv.files)
    time.sleep(0.9)
    mock_hook.state.update(state="idle", file=None, message=msg)
    assert _notice(ws) == msg
    st = _info(ws, lambda st: st["Status"] == 8)
    assert st["ErrorNumber"] == en
    ws.close()


def test_message_without_bridge_start_ignored(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    mock_hook.state.update(message="file check failed")       # a touchscreen start failed, not ours
    assert _notices(ws, 1.2) == []
    ws.close()


def _status_now(ws):
    ws.msgs.clear()
    assert _ack(ws, 0)["Ack"] == 0
    return ws.wait_topic("status")["Status"]["PrintInfo"]


def test_start_that_fails_before_any_poll(gksdcp_start, mock_hook):
    # a corrupt file fails gkhook's check before the next status poll: no snapshot ever shows starting
    srv, ws = _start(gksdcp_start)
    mock_hook.on_cmd["start"] = lambda st: st.update(state="idle", file=None, message="file check failed")
    for attempt in (1, 2):                         # the same failure twice: the second one is announced too
        ws.msgs.clear()
        assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 0
        assert _notices(ws, 1.5) == ["file check failed"]
        st = _status_now(ws)
        assert st["Status"] == 8 and st["ErrorNumber"] == 2 and st["TaskId"] == ""
        ids = _ack(ws, 320)["HistoryData"]
        assert len(ids) == attempt
        task = _ack(ws, 321, {"Id": ids[:1]})["HistoryDetailList"][0]
        assert task["TaskStatus"] == 3 and task["EndTime"] > 0
    ws.close()


@pytest.mark.parametrize("cmd,hk", [(129, "pause"), (131, "resume"), (130, "stop")])
def test_pause_resume_stop_forwarded(gksdcp_start, mock_hook, cmd, hk):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, cmd)["Ack"] == 0
    assert _hook_cmds(mock_hook, hk) == [{"cmd": hk}]
    ws.close()


@pytest.mark.parametrize("cmd,hk", [(129, "pause"), (131, "resume"), (130, "stop")])
def test_stop_error_nonzero_ack_and_notice(gksdcp_start, mock_hook, cmd, hk):
    srv, ws = _start(gksdcp_start)
    mock_hook.errors[hk] = "not printing"
    assert _ack(ws, cmd)["Ack"] == 1
    assert _notice(ws) == "not printing"
    ws.close()


def test_not_applicable_cmds(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    mock_hook.received.clear()
    assert _ack(ws, 132)["Ack"] == 1
    assert _ack(ws, 133)["Ack"] == 1
    assert all(r.get("cmd") == "status" for r in mock_hook.received)
    ws.close()


def test_camera_ack_2(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    d = _ack(ws, 386)
    assert d["Ack"] == 2 and d["VideoUrl"] == ""
    ws.close()


def test_timelapse_ack_1(gksdcp_start):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 387)["Ack"] == 1
    ws.close()


@pytest.mark.parametrize("name", ["a\\b.ctb", "/local/a\\b.ctb", "/local/d\\/a.ctb"])
def test_start_backslash_name_refused(gksdcp_start, mock_hook, name):
    assert "\\" in name and "\x08" not in name
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": name})["Ack"] == 2
    assert _notice(ws) == "bad file path"
    assert _hook_cmds(mock_hook, "start") == []
    ws.close()
