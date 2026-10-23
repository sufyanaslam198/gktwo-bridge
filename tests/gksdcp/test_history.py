import time

import pytest

from sdcp_client import ws_connect
from test_status import settle


def _status(ws, pred, timeout=3.0):
    end = time.monotonic() + timeout
    while True:
        m = ws.wait_topic("status", timeout=max(0.05, end - time.monotonic()))
        if pred(m):
            return m


def _data(ws, cmd, data=None):
    r = ws.wait_response(ws.request(cmd, data), timeout=2.0)
    return r["Data"]["Data"]


def _print(mock_hook, state, file="/media/emmc/job.ctb", layer=1):
    mock_hook.state.update(state=state, file=file if state != "idle" else None, layer=layer, total_layers=10,
                           phase="exposing" if state == "printing" else None)


def _run_print(ws, mock_hook, end_state="complete"):
    """idle -> printing -> end_state -> idle, as the touchscreen would do it. Returns the TaskId."""
    _print(mock_hook, "printing")
    st = _status(ws, lambda m: m["Status"]["PrintInfo"]["TaskId"] != "")
    tid = st["Status"]["PrintInfo"]["TaskId"]
    time.sleep(0.6)
    _print(mock_hook, end_state, layer=10)
    _status(ws, lambda m: m["Status"]["CurrentStatus"] == [0])
    time.sleep(0.6)
    _print(mock_hook, "idle")
    _status(ws, lambda m: m["Status"]["PrintInfo"]["TaskId"] == "")
    return tid


def test_cmd320_empty_then_task_id(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    d = _data(ws, 320)
    assert d["Ack"] == 0 and d["HistoryData"] == []
    settle(ws)
    _print(mock_hook, "printing")
    st = _status(ws, lambda m: m["Status"]["PrintInfo"]["TaskId"] != "")
    tid = st["Status"]["PrintInfo"]["TaskId"]
    assert len(tid) == 36
    assert _data(ws, 320)["HistoryData"] == [tid]
    ws.close()


def test_touchscreen_print_creates_finished_task(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    tid = _run_print(ws, mock_hook)
    items = _data(ws, 321, {"Id": [tid]})["HistoryDetailList"]
    assert len(items) == 1
    t = items[0]
    assert t["TaskId"] == tid and t["TaskStatus"] == 1 and t["EndTime"] > 0 and t["BeginTime"] > 0
    assert t["TaskName"] == "job.ctb" and t["AlreadyPrintLayer"] == 10
    assert t["Thumbnail"] == "" and t["MD5"] == "" and t["SliceInformation"] == {}
    assert t["ErrorStatusReason"] == 0 and t["TimeLapseVideoStatus"] == 0 and t["TimeLapseVideoUrl"] == ""
    assert t["CurrentLayerTalVolume"] == 0
    ws.close()


def test_error_and_stop_statuses(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    bad = _run_print(ws, mock_hook, "error")
    stopped = _run_print(ws, mock_hook, "idle")
    ids = _data(ws, 320)["HistoryData"]
    assert ids == [stopped, bad]                       # newest first
    items = _data(ws, 321, {"Id": ids})["HistoryDetailList"]
    assert [(i["TaskId"], i["TaskStatus"], i["ErrorStatusReason"]) for i in items] == [(stopped, 3, 0), (bad, 2, 19)]
    assert all(i["EndTime"] > 0 for i in items)
    ws.close()


def test_cmd321_key_variants(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    tid = _run_print(ws, mock_hook)
    for data in ({"Id": [tid]}, {"TaskIdList": [tid]}, {"TaskIdList": [tid], "Id": [tid]},
                 {"Id": [tid, "unknown-id", tid]}):
        d = _data(ws, 321, data)
        assert d["Ack"] == 0 and [i["TaskId"] for i in d["HistoryDetailList"]] == [tid], data
    d = _data(ws, 321, {"Id": ["unknown-id"]})
    assert d["Ack"] == 0 and d["HistoryDetailList"] == []
    for data in ({}, {"Id": "x"}, {"Id": {"a": 1}}, {"Id": 7}, {"TaskIdList": None}):
        d = _data(ws, 321, data)
        assert d["Ack"] == 1 and d["HistoryDetailList"] == [], data
    ws.close()


def test_history_survives_restart(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    tid = _run_print(ws, mock_hook)
    ws.close()
    srv.proc.terminate()
    srv.proc.wait(5)
    srv2 = gksdcp_start()
    ws = ws_connect(srv2.ws_port)
    assert _data(ws, 320)["HistoryData"] == [tid]
    ws.close()


def test_restart_during_print_resumes_task(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    _print(mock_hook, "printing")
    tid = _status(ws, lambda m: m["Status"]["PrintInfo"]["TaskId"] != "")["Status"]["PrintInfo"]["TaskId"]
    ws.close()
    srv.proc.terminate()
    srv.proc.wait(5)
    srv2 = gksdcp_start()
    ws = ws_connect(srv2.ws_port)
    assert _data(ws, 320)["HistoryData"] == [tid]
    ws.request(0)
    assert _status(ws, lambda m: True)["Status"]["PrintInfo"]["TaskId"] == tid
    ws.close()


def test_restart_after_print_ended_closes_orphan(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    _print(mock_hook, "printing")
    tid = _status(ws, lambda m: m["Status"]["PrintInfo"]["TaskId"] != "")["Status"]["PrintInfo"]["TaskId"]
    ws.close()
    srv.proc.terminate()
    srv.proc.wait(5)
    _print(mock_hook, "idle")
    srv2 = gksdcp_start()
    ws = ws_connect(srv2.ws_port)
    t = _data(ws, 321, {"Id": [tid]})["HistoryDetailList"][0]
    assert t["TaskStatus"] == 3 and t["EndTime"] > 0
    ws.close()
