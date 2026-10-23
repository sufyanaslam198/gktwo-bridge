import json
import socket
import time

import pytest

from sdcp_client import ws_connect

MID = "0000aabbccddeeff"


def settle(ws, quiet=0.7):
    """Drop whatever the server pushes on its own until it has been quiet for a while."""
    end = time.monotonic() + quiet
    while time.monotonic() < end:
        try:
            ws.recv(max(0.05, end - time.monotonic()))
        except socket.timeout:
            pass
    ws.msgs.clear()


def test_cmd0_response_then_status_push(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    rid = ws.request(0)
    r = ws.wait_response(rid)
    assert r["Data"]["Cmd"] == 0 and r["Data"]["RequestID"] == rid
    assert r["Data"]["Data"]["Ack"] == 0
    assert r["Data"]["MainboardID"] == MID and r["Topic"] == "sdcp/response/" + MID
    assert len(r["Id"]) == 32 and r["Data"]["TimeStamp"] > 0
    st = ws.wait_topic("status")
    assert st["Topic"] == "sdcp/status/" + MID and st["MainboardID"] == MID
    assert st["Status"]["CurrentStatus"] == [0]
    assert st["Status"]["PrintInfo"]["Status"] == 0
    assert st["Status"]["GKBridge"]["Z"] == {"Pos": None, "Homed": False, "Moving": False}
    assert st["Status"]["GKBridge"]["HookOk"] is True
    ws.close()


def test_cmd1_attributes_push(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    rid = ws.request(1)
    assert ws.wait_response(rid)["Data"]["Data"]["Ack"] == 0
    a = ws.wait_topic("attributes")
    assert a["Topic"] == "sdcp/attributes/" + MID
    at = a["Attributes"]
    assert at["MainboardID"] == MID and at["ProtocolVersion"] == "V3.0.0"
    assert at["Resolution"] == "7680x4320" and at["XYZsize"] == "228.1x128.3x245"
    assert at["Capabilities"] == ["FILE_TRANSFER", "PRINT_CONTROL"] and at["SupportFileType"] == ["CTB"]
    assert set(at["DevicesStatus"]) == {"TempSensorStatusOfUVLED", "LCDStatus", "SgStatus", "ZMotorStatus",
                                        "RotateMotorStatus", "RelaseFilmState", "XMotorStatus"}
    assert at["RemainingMemory"] > 0
    ws.close()


def test_unknown_cmd_nonzero_ack(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    rid = ws.request(9999)
    r = ws.wait_response(rid)
    assert r["Data"]["Cmd"] == 9999 and r["Data"]["Data"]["Ack"] != 0
    ws.close()


def test_wrong_mainboard_id_still_answered(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    rid = ws.request(0, mainboard_id="ffffffffffffffff")
    assert ws.wait_response(rid)["Data"]["Data"]["Ack"] == 0
    ws.close()


def test_status_push_on_change(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    mock_hook.state["state"] = "printing"
    mock_hook.state["phase"] = "exposing"
    mock_hook.state["file"] = "/media/emmc/x.ctb"
    mock_hook.state["layer"] = 3
    st = ws.wait_topic("status", timeout=1.0)        # unprompted, within 1 s
    assert st["Status"]["CurrentStatus"] == [1]
    assert st["Status"]["PrintInfo"]["Status"] == 3
    assert st["Status"]["PrintInfo"]["Filename"] == "/local/x.ctb"
    ws.close()


def test_status_push_last_state_always_sent(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    for layer in range(1, 6):                        # faster than the rate limit
        mock_hook.state["state"] = "printing"
        mock_hook.state["layer"] = layer
        time.sleep(0.3)
    mock_hook.state["layer"] = 99
    deadline = time.monotonic() + 2.0
    last = None
    while time.monotonic() < deadline:
        try:
            m = ws.wait_topic("status", timeout=0.5)
        except socket.timeout:
            break
        last = m
    assert last is not None and last["Status"]["PrintInfo"]["CurrentLayer"] == 99
    ws.close()


def _poll(ws, cmd):
    ws.send_ping(b"")                                # a WebSocket protocol ping between requests
    t0 = time.monotonic()
    rid = ws.request(cmd)
    assert ws.wait_response(rid, timeout=1.0)["Data"]["Data"]["Ack"] == 0
    assert time.monotonic() - t0 < 1.0


def test_ha_poll_sequence(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    _poll(ws, 1)
    _poll(ws, 0)
    ws.close()


def test_ha_poll_cmd320(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    _poll(ws, 320)
    ws.close()


def test_any_integer_cmd_is_answered(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    for cmd in (-1, 70000, 2147483647):
        r = ws.wait_response(ws.request(cmd), timeout=1.0)
        assert r["Data"]["Cmd"] == cmd and r["Data"]["Data"]["Ack"] != 0
    ws.close()


def test_request_id_lengths(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    for n in (100, 256):                             # echoed exactly
        rid = "r" * n
        assert ws.wait_response(ws.request(0, rid=rid), timeout=1.0)["Data"]["RequestID"] == rid
    ws.msgs.clear()
    long_rid = "q" * 300                             # unreadable: no response, connection stays open
    ws.request(0, rid=long_rid)
    with pytest.raises(socket.timeout):
        ws.wait_response(long_rid, timeout=0.5)
    rid = ws.request(0)
    assert ws.wait_response(rid, timeout=1.0)["Data"]["Data"]["Ack"] == 0
    ws.close()


def test_garbage_requests_keep_connection(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    good = {"Cmd": 0, "Data": {}, "RequestID": "r", "MainboardID": MID, "TimeStamp": 1}
    garbage = [
        "", "{", "null", "[]", "[1,2,3]", '"x"', "12", "{}", '{"Data":1}', '{"Data":[]}', '{"Data":{}}',
        '{"Data":{"Cmd":"0"}}', '{"Data":{"Cmd":0.5}}', '{"Data":{"Cmd":1e3}}', '{"Data":{"Cmd":null}}',
        '{"Data":{"Cmd":99999999999999999999}}', '{"Data":{"Cmd":-1}}',
        '{"Data":{"Cmd":9999,"RequestID":7}}', '{"Data":{"Cmd":9999,"RequestID":"%s"}}' % ("x" * 300),
        '{"Data":{"Cmd":9999,"Data":"str","TimeStamp":"now"}}',
        '{"Data":{"Cmd":0,"TimeStamp":-5}}', '{"Data":{"Cmd":0,"TimeStamp":99999999999999999999}}',
        "[" * 300 + "]" * 300, '{"Data":' * 100 + "1" + "}" * 100,
        json.dumps({"Data": {"Cmd": 0, "Pad": "x" * 60000}}),
        "\xff\xfe not utf8 \x00",
    ]
    for g in garbage:
        ws.send_text(g.encode("latin-1", "replace") if isinstance(g, str) else g)
    ws.send_frame(2, b"\x00\x01\x02")                # binary frames are ignored
    ws.send_ping(b"hi")
    rid = ws.request(0)
    deadline = time.monotonic() + 3.0
    while True:                                      # replies to the odd request are fine, ours must come
        try:
            ws.wait_response(rid, timeout=max(0.1, deadline - time.monotonic()))
            break
        except socket.timeout:
            pytest.fail("connection stopped answering after garbage")
    # a second connection still works too
    ws2 = ws_connect(srv.ws_port)
    assert ws2.wait_response(ws2.request(0))["Data"]["Data"]["Ack"] == 0
    ws2.close()
    ws.close()
    assert srv.proc.poll() is None


def test_hook_absent_still_answers(gksdcp_start, mock_hook):
    mock_hook.mode = "absent"
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    assert ws.wait_response(ws.request(1))["Data"]["Data"]["Ack"] == 0
    rid = ws.request(0)
    assert ws.wait_response(rid)["Data"]["Data"]["Ack"] == 0
    st = ws.wait_topic("status")
    assert st["Status"]["CurrentStatus"] == [0]
    assert st["Status"]["GKBridge"]["Message"].startswith("hook: ")
    assert st["Status"]["GKBridge"]["HookOk"] is False
    t0 = time.monotonic()
    r = ws.wait_response(ws.request(129, {"Filename": "/local/x.ctb", "StartLayer": 0}), timeout=2.5)
    assert r["Data"]["Data"]["Ack"] == 1          # 129 is a real command now: the hook path fails, not "unknown"
    assert time.monotonic() - t0 < 2.5
    assert ws.wait_topic("notice")["Data"]["Data"]["Message"]
    ws.close()


def test_hook_hung_still_answers(gksdcp_start, mock_hook):
    mock_hook.mode = "hang"
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    t0 = time.monotonic()
    assert ws.wait_response(ws.request(0), timeout=1.0)["Data"]["Data"]["Ack"] == 0
    assert time.monotonic() - t0 < 1.0
    st = ws.wait_topic("status")
    assert st["Status"]["GKBridge"]["Message"].startswith("hook: ")
    assert st["Status"]["GKBridge"]["HookOk"] is False
    ws.close()


def test_mass_close_with_close_hook_broadcasting(monkeypatch, gksdcp_start):
    """Regression for the Task 3 sweep fix: an on_conn_close user that broadcasts while several
    connections die in one pass. The debug-only env var makes the SDCP layer send a notice per close."""
    monkeypatch.setenv("GKSDCP_TEST_CLOSE_NOTICE", "1")
    srv = gksdcp_start()
    survivor = ws_connect(srv.ws_port)
    victims = [ws_connect(srv.ws_port) for _ in range(8)]
    settle(survivor, 0.3)
    for v in victims:                                # abrupt, all in the same moment
        v.sock.close()
    got = 0
    deadline = time.monotonic() + 3.0
    while got < 8 and time.monotonic() < deadline:
        try:
            n = survivor.wait_topic("notice", timeout=0.5)
        except socket.timeout:
            continue
        assert n["Data"]["Data"]["Type"] == 100
        got += 1
    assert got == 8
    assert srv.proc.poll() is None
    assert survivor.wait_response(survivor.request(0))["Data"]["Data"]["Ack"] == 0
    survivor.close()


def _wait_status(ws, pred, timeout=2.0):
    end = time.monotonic() + timeout
    while True:
        m = ws.wait_topic("status", timeout=max(0.05, end - time.monotonic()))
        if pred(m):
            return m


def _drive(ws, mock_hook, states):
    for st in states:
        mock_hook.state["state"] = st
        mock_hook.state["phase"] = "exposing" if st == "printing" else None
        _wait_status(ws, lambda m, st=st: m["Status"]["CurrentStatus"] == ([1] if st in ("printing", "paused", "starting") else [0]))
        time.sleep(0.6)                              # stay clear of the push rate limit


def test_fresh_boot_sub_status_zero(gksdcp_start):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    ws.request(0)
    st = ws.wait_topic("status")
    assert st["Status"]["PrintInfo"]["Status"] == 0 and st["Status"]["PreviousStatus"] == 0
    ws.close()


def test_stop_latches_sub_status_8(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    _drive(ws, mock_hook, ["printing", "idle"])
    ws.request(0)
    st = _wait_status(ws, lambda m: m["Status"]["CurrentStatus"] == [0])
    assert st["Status"]["PrintInfo"]["Status"] == 8 and st["Status"]["PreviousStatus"] == 1
    ws.close()


def test_complete_latches_sub_status_9(gksdcp_start, mock_hook):
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    settle(ws)
    _drive(ws, mock_hook, ["printing", "complete", "idle"])
    ws.request(0)
    st = _wait_status(ws, lambda m: m["Status"]["CurrentStatus"] == [0])
    assert st["Status"]["PrintInfo"]["Status"] == 9 and st["Status"]["PreviousStatus"] == 1
    ws.close()
